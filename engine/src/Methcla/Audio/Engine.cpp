// Copyright 2012-2013 Samplecount S.L.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Methcla/Audio/Engine.hpp"
#include "Methcla/Audio/Group.hpp"
#include "Methcla/Audio/Synth.hpp"
#include "Methcla/Exception.hpp"
#include "Methcla/Utility/MessageQueue.hpp"

#include <boost/heap/priority_queue.hpp>
#include <cstdlib>
#include <iostream>
#include <oscpp/print.hpp>

using namespace Methcla;
using namespace Methcla::Audio;
using namespace Methcla::Memory;

// OSC request with reference counting.
class Request
{
    struct Data
    {
        size_t  m_refs;
        void*   m_packet;
    };

public:
    Request()
        : m_env(nullptr)
        , m_data(nullptr)
        , m_size(0)
    {
    }

    Request(Environment* env, const void* packet, size_t size)
        : m_env(env)
        , m_size(size)
    {
        // Allocate memory for packet and data block
        char* mem = Memory::allocOf<char>(sizeof(Data) + size);
        memcpy(mem + sizeof(Data), packet, size);
        m_data = reinterpret_cast<Data*>(mem);
        m_data->m_refs = 1;
        m_data->m_packet = mem + sizeof(Data);
    }

    Request(const Request& other)
        : Request()
    {
        *this = other;
    }

    Request& operator=(const Request& other)
    {
        if (this != &other) {
            release();
            m_env = other.m_env;
            m_data = other.m_data;
            m_size = other.m_size;
            retain();
        }
        return *this;
    }

    ~Request()
    {
        release();
    }

    void* packet()
    {
        return m_data == nullptr ? nullptr : m_data->m_packet;
    }

    size_t size() const
    {
        return m_size;
    }

private:
    void retain()
    {
        if (m_data != nullptr)
            m_data->m_refs++;
    }

    static void perform_free_data(Environment*, CommandData* data)
    {
        Methcla::Memory::free(data);
    }

    void release()
    {
        if (m_data != nullptr) {
            m_data->m_refs--;
            if (m_data->m_refs == 0) {
                m_env->sendToWorker(perform_free_data, m_data);
                m_env = nullptr;
                m_data = nullptr;
                m_size = 0;
            }
        }
    }

private:
    Environment* m_env;
    Data*        m_data;
    size_t       m_size;
};

struct Command
{
    void perform()
    {
        if (m_perform != nullptr)
            m_perform(m_env, m_data);
    }

    Environment* m_env;
    PerformFunc  m_perform;
    void*        m_data;
};

class ScheduleItem
{
public:
    ScheduleItem(Methcla_Time time, const Request& request)
        : m_time(time)
        , m_request(request)
    { }

    Methcla_Time time() const
    {
        return m_time;
    }

    const Request& request() const
    {
        return m_request;
    }

    bool operator==(const ScheduleItem& other) const
    {
        return time() == other.time();
    }

    bool operator<(const ScheduleItem& other) const
    {
        return time() > other.time();
    }

private:
    Methcla_Time    m_time;
    Request         m_request;
};

class Scheduler
{
    typedef boost::heap::priority_queue<
        ScheduleItem,
        boost::heap::stable<true>,
        boost::heap::stability_counter_type<uint64_t>
        > PriorityQueue;

    // We need constant time size and reserve in order to avoid memory allocations from the audio thread.
    static_assert(PriorityQueue::has_reserve, "priority queue does not implement reserve()");
    static_assert(PriorityQueue::constant_time_size, "priority queue implementation has non-constant time size()");
    // We want a stable priority queue
    static_assert(PriorityQueue::is_stable, "priority queue implementation is not stable");

    size_t        m_maxSize;
    PriorityQueue m_queue;

public:
    Scheduler(size_t maxSize)
        : m_maxSize(maxSize)
    {
        m_queue.reserve(m_maxSize);
    }

    void push(Methcla_Time time, const Request& request)
    {
        if (m_queue.size() < m_maxSize) {
            m_queue.push(ScheduleItem(time, request));
        } else {
            throw std::runtime_error("Scheduler queue overflow");
        }
    }

    bool isEmpty() const
    {
        return m_queue.empty();
    }

    Methcla_Time time() const
    {
        assert(!isEmpty());
        return m_queue.top().time();
    }

    Request top() const
    {
        assert(!isEmpty());
        return m_queue.top().request();
    }

    void pop()
    {
        assert(!isEmpty());
        m_queue.pop();
    }
};

class Methcla::Audio::EnvironmentImpl
{
public:
    struct ErrorData
    {
        int32_t requestId;
        char*   message;
    };

    static const size_t kQueueSize = 8192;

    EnvironmentImpl(size_t realtimeMemorySize)
        : m_epoch(0)
        , m_rtMem(realtimeMemorySize)
        , m_requests(kQueueSize)
        , m_worker(kQueueSize, 2)
        , m_scheduler(kQueueSize)
    { }

    typedef Utility::MessageQueue<Request> MessageQueue;
    typedef Utility::WorkerThread<Command> Worker;

    std::vector<std::shared_ptr<ExternalAudioBus>>  m_externalAudioInputs;
    std::vector<std::shared_ptr<ExternalAudioBus>>  m_externalAudioOutputs;
    std::vector<std::shared_ptr<AudioBus>>          m_internalAudioBuses;
    Epoch                                           m_epoch;

    Memory::RTMemoryManager         m_rtMem;
    MessageQueue                    m_requests;
    Worker                          m_worker;
    Scheduler                       m_scheduler;
};

extern "C" {

static void methcla_api_host_register_synthdef(const Methcla_Host* host, const Methcla_SynthDef* synthDef)
{
    assert(host && host->handle);
    assert(synthDef);
    return static_cast<Environment*>(host->handle)->registerSynthDef(synthDef);
}

static const Methcla_SoundFileAPI* methcla_api_host_get_soundfile_api(const Methcla_Host* host, const char* mimeType)
{
    assert(host && host->handle);
    assert(mimeType);
    return static_cast<Environment*>(host->handle)->soundFileAPI(mimeType);
}

static double methcla_api_world_samplerate(const Methcla_World* world)
{
    assert(world && world->handle);
    return static_cast<Environment*>(world->handle)->sampleRate();
}

static size_t methcla_api_world_block_size(const Methcla_World* world)
{
    assert(world && world->handle);
    return static_cast<Environment*>(world->handle)->blockSize();
}

static void* methcla_api_world_alloc(const Methcla_World* world, size_t size)
{
    assert(world && world->handle);
    try {
        return static_cast<Environment*>(world->handle)->rtMem().alloc(size);
    } catch (std::invalid_argument) {
    } catch (std::bad_alloc) {
    }
    return nullptr;
}

static void* methcla_api_world_alloc_aligned(const Methcla_World* world, size_t alignment, size_t size)
{
    assert(world && world->handle);
    try {
        return static_cast<Environment*>(world->handle)->rtMem().allocAligned(alignment, size);
    } catch (std::invalid_argument) {
    } catch (std::bad_alloc) {
    }
    return nullptr;
}

static void methcla_api_world_free(const Methcla_World* world, void* ptr)
{
    assert(world && world->handle);
    return static_cast<Environment*>(world->handle)->rtMem().free(ptr);
}

static void methcla_api_world_resource_retain(const Methcla_World*, Methcla_Resource* resource)
{
    assert(resource);
    static_cast<Reference*>(resource)->retain();
}

static void methcla_api_world_resource_release(const Methcla_World*, Methcla_Resource* resource)
{
    assert(resource);
    static_cast<Reference*>(resource)->release();
}

static void methcla_api_host_perform_command(const Methcla_Host*, Methcla_WorldPerformFunction, void*);
static void methcla_api_world_perform_command(const Methcla_World*, Methcla_HostPerformFunction, void*);

} // extern "C"

Environment::Environment(PacketHandler handler, const Options& options)
    : m_sampleRate(options.sampleRate)
    , m_blockSize(options.blockSize)
    , m_listener(handler)
    , m_nodes(options.maxNumNodes)
{
    m_impl = new EnvironmentImpl(options.realtimeMemorySize);

    m_rootNode = Group::construct(*this, NodeId(0), nullptr, Node::kAddToTail);
    m_nodes.insert(m_rootNode->id(), m_rootNode);

    const Epoch prevEpoch = epoch() - 1;

    m_impl->m_externalAudioInputs.reserve(options.numHardwareInputChannels);
    for (size_t i=0; i < options.numHardwareInputChannels; i++)
    {
        m_impl->m_externalAudioInputs.push_back(
            std::make_shared<ExternalAudioBus>(prevEpoch)
        );
    }

    m_impl->m_externalAudioOutputs.reserve(options.numHardwareOutputChannels);
    for (size_t i=0; i < options.numHardwareOutputChannels; i++)
    {
        m_impl->m_externalAudioOutputs.push_back(
            std::make_shared<ExternalAudioBus>(prevEpoch)
        );
    }

    for (size_t i=0; i < options.maxNumAudioBuses; i++)
    {
        m_impl->m_internalAudioBuses.push_back(
            std::make_shared<InternalAudioBus>(blockSize(), prevEpoch)
        );
    }

    // Initialize Methcla_Host interface
    m_host = {
        this,
        methcla_api_host_register_synthdef,
        methcla_api_host_get_soundfile_api,
        methcla_api_host_perform_command
    };

    // Initialize Methcla_World interface
    m_world = {
        this,
        methcla_api_world_samplerate,
        methcla_api_world_block_size,
        methcla_api_world_alloc,
        methcla_api_world_alloc_aligned,
        methcla_api_world_free,
        methcla_api_world_perform_command,
        methcla_api_world_resource_retain,
        methcla_api_world_resource_release
    };
}

Environment::~Environment()
{
}

AudioBus* Environment::audioBus(AudioBusId id)
{
    return m_impl->m_internalAudioBuses.at(id).get();
}

size_t Environment::numExternalAudioOutputs() const
{
    return m_impl->m_externalAudioOutputs.size();
}

size_t Environment::numExternalAudioInputs() const
{
    return m_impl->m_externalAudioInputs.size();
}

AudioBus* Environment::externalAudioOutput(AudioBusId id)
{
    return m_impl->m_externalAudioOutputs.at(id).get();
}

AudioBus* Environment::externalAudioInput(AudioBusId id)
{
    return m_impl->m_externalAudioInputs.at(id).get();
}

Memory::RTMemoryManager& Environment::rtMem()
{
    return m_impl->m_rtMem;
}

Epoch Environment::epoch() const
{
    return m_impl->m_epoch;
}

void Environment::send(const void* packet, size_t size)
{
    m_impl->m_requests.send(Request(this, packet, size));
}

void Environment::sendToWorker(PerformFunc f, void* data)
{
    Command cmd;
    cmd.m_env = this;
    cmd.m_perform = f;
    cmd.m_data = data;
    m_impl->m_worker.sendToWorker(cmd);
}

void Environment::sendFromWorker(PerformFunc f, void* data)
{
    Command cmd;
    cmd.m_env = this;
    cmd.m_perform = f;
    cmd.m_data = data;
    m_impl->m_worker.sendFromWorker(cmd);
}

void Environment::process(Methcla_Time currentTime, size_t numFrames, const sample_t* const* inputs, sample_t* const* outputs)
{
    BOOST_ASSERT_MSG( numFrames <= blockSize(), "numFrames exceeds blockSize()" );

    // Process external requests
    processRequests(currentTime);
    // Process scheduled requests
    processScheduler(currentTime, currentTime + numFrames / sampleRate());
    // std::cout << "Environment::process " << currentTime << std::endl;

    // Process non-realtime commands
    m_impl->m_worker.perform();

    const size_t numExternalInputs = numExternalAudioInputs();
    const size_t numExternalOutputs = numExternalAudioOutputs();

    // Connect input and output buses
    for (size_t i=0; i < numExternalInputs; i++) {
        m_impl->m_externalAudioInputs[i]->setData(const_cast<sample_t*>(inputs[i]));
        m_impl->m_externalAudioInputs[i]->setEpoch(epoch());
    }
    for (size_t i=0; i < numExternalOutputs; i++) {
        m_impl->m_externalAudioOutputs[i]->setData(outputs[i]);
    }

    // Run DSP graph
    m_rootNode->process(numFrames);

    // Zero outputs that haven't been written to
    for (size_t i=0; i < numExternalOutputs; i++) {
        if (m_impl->m_externalAudioOutputs[i]->epoch() != epoch()) {
            memset(outputs[i], 0, numFrames * sizeof(sample_t));
        }
    }

    m_impl->m_epoch++;
}

static void perform_nrt_free(Environment*, CommandData* data)
{
    Methcla::Memory::free(data);
}

static void perform_rt_free(Environment* env, CommandData* data)
{
    env->rtMem().free(data);
}

void Environment::perform_response_ack(Environment* env, CommandData* data)
{
    const char address[] = "/ack";
    const size_t numArgs = 1;
    const size_t packetSize = OSCPP::Size::message(address, numArgs)
                                + OSCPP::Size::int32();
    const int32_t requestId = *static_cast<int32_t*>(data);
    OSCPP::Client::StaticPacket<packetSize> packet;
    // OSCPP::Client::DynamicPacket packet(kPacketSize);
    packet
        .openMessage(address, numArgs)
            .int32(requestId)
        .closeMessage();
    env->reply(requestId, packet);
    env->sendFromWorker(perform_rt_free, data);
}

void Environment::perform_response_nodeId(Environment* env, CommandData* data)
{
    const char address[] = "/ack";
    const size_t numArgs = 2;
    const size_t packetSize = OSCPP::Size::message(address, numArgs)
                                + 2 * OSCPP::Size::int32();
    const int32_t requestId = ((int32_t*)data)[0];
    const int32_t nodeId = ((int32_t*)data)[1];
    OSCPP::Client::StaticPacket<packetSize> packet;
    // OSCPP::Client::DynamicPacket packet(kPacketSize);
    packet
        .openMessage(address, numArgs)
            .int32(requestId)
            .int32(nodeId)
        .closeMessage();
    env->reply(requestId, packet);
    env->sendFromWorker(perform_rt_free, data);
}

void Environment::perform_response_query_external_inputs(Environment* env, CommandData* data)
{
    // const char address[] = "/ack";
    // const size_t numBuses = env->numExternalAudioInputs();
    // const size_t numArgs = 1 + numBuses;
    // const size_t packetSize = OSCPP::Size::message(address, numArgs) + numArgs * OSCPP::Size::int32();
    // OSCPP::Client::DynamicPacket packet(packetSize);
    // packet.openMessage(address, numArgs);
    // packet.int32(data->response.requestId);
    // for (size_t i=0; i < numBuses; i++) {
    //     packet.int32(env->externalAudioInput(i).id());
    // }
    // packet.closeMessage();
    // env->reply(data->response.requestId, packet);
}

void Environment::perform_response_query_external_outputs(Environment* env, CommandData* data)
{
    // const char address[] = "/ack";
    // const size_t numBuses = env->numExternalAudioOutputs();
    // const size_t numArgs = 1 + numBuses;
    // const size_t packetSize = OSCPP::Size::message(address, numArgs) +  numArgs * OSCPP::Size::int32();
    // OSCPP::Client::DynamicPacket packet(packetSize);
    // packet.openMessage(address, numArgs);
    // packet.int32(data->response.requestId);
    // for (size_t i=0; i < numBuses; i++) {
    //     packet.int32(env->externalAudioOutput(i).id());
    // }
    // packet.closeMessage();
    // env->reply(data->response.requestId, packet);
}

void Environment::perform_response_error(Environment* env, CommandData* commandData)
{
    EnvironmentImpl::ErrorData* data = (EnvironmentImpl::ErrorData*)commandData;
    const char address[] = "/error";
    const size_t numArgs = 2;
    const size_t packetSize = OSCPP::Size::message(address, numArgs)
                             + OSCPP::Size::int32()
                             + OSCPP::Size::string(strlen(data->message));
    OSCPP::Client::DynamicPacket packet(packetSize);
    packet
        .openMessage(address, numArgs)
            .int32(data->requestId)
            .string(data->message)
        .closeMessage();
    env->reply(data->requestId, packet);
    env->sendFromWorker(perform_rt_free, data);
}

void Environment::replyError(Methcla_RequestId requestId, const char* msg)
{
    EnvironmentImpl::ErrorData* data =
        (EnvironmentImpl::ErrorData*)rtMem().alloc(sizeof(EnvironmentImpl::ErrorData)+strlen(msg)+1);
    data->requestId = requestId;
    data->message = (char*)data + sizeof(EnvironmentImpl::ErrorData);
    strcpy(data->message, msg);
    sendToWorker(perform_response_error, data);
}

void Environment::processRequests(Methcla_Time currentTime)
{
    Request request;
    while (m_impl->m_requests.next(request)) {
        try {
            OSCPP::Server::Packet packet(request.packet(), request.size());
            if (packet.isBundle()) {
                OSCPP::Server::Bundle bundle(packet);
                if (processBundle(bundle)) {
                    if (bundle.time() == 1) {
                        processBundle(packet, currentTime, currentTime);
                    } else {
                        m_impl->m_scheduler.push(methcla_time_from_uint64(bundle.time()), request);
                    }
                }
            } else {
                if (processMessage(packet))
                    processMessage(packet, currentTime, currentTime);
            }
        } catch (std::exception& e) {
            std::cerr << "Unhandled exception in `processRequests': " << e.what() << std::endl;
        }
    }
}

void Environment::processScheduler(Methcla_Time currentTime, Methcla_Time nextTime)
{
    while (!m_impl->m_scheduler.isEmpty()) {
        Methcla_Time scheduleTime = m_impl->m_scheduler.time();
        if (scheduleTime <= nextTime) {
            Request request(m_impl->m_scheduler.top());
            OSCPP::Server::Packet packet(request.packet(), request.size());
            if (packet.isBundle()) {
                processBundle(packet, scheduleTime, currentTime);
            } else {
                processMessage(packet, scheduleTime, currentTime);
            }
            m_impl->m_scheduler.pop();
        } else {
            break;
        }
    }
}

bool Environment::processBundle(const OSCPP::Server::Bundle& bundle)
{
    auto packets = bundle.packets();
    bool needsScheduling = false;
    while (!packets.atEnd()) {
        auto p = packets.next();
        needsScheduling |= p.isBundle() ? processBundle(p) : processMessage(p);
    }
    return needsScheduling;
}

void Environment::processBundle(const OSCPP::Server::Bundle& bundle, Methcla_Time scheduleTime, Methcla_Time currentTime)
{
    auto packets = bundle.packets();
    while (!packets.atEnd()) {
        auto p = packets.next();
        if (p.isBundle()) {
            // NOTE: Nested bundles are flattened
            processBundle(p, scheduleTime, currentTime);
        } else {
            processMessage(p, scheduleTime, currentTime);
        }
    }
}

bool Environment::processMessage(const OSCPP::Server::Message& msg)
{
#if 0
    std::cerr << "Request (recv): " << msg << std::endl;
#endif

    auto args = msg.args();
    // Methcla_RequestId requestId = args.int32();

    try {
        if (msg == "/group/new") {
            NodeId nodeId = NodeId(args.int32());
            NodeId targetId = NodeId(args.int32());
            args.drop(); // int32_t addAction = args.int32();

            Node* targetNode = m_nodes.lookup(targetId).get();
            if (targetNode == nullptr)
                throw Error(kMethcla_NodeIdError);
            Group* targetGroup = targetNode->isGroup() ? dynamic_cast<Group*>(targetNode)
                                                       : dynamic_cast<Synth*>(targetNode)->parent();
            Group* group = Group::construct(*this, nodeId, targetGroup, Node::kAddToTail);
            nodes().insert(group->id(), group);

            // No scheduling needed.
            return false;
        } else if (msg == "/synth/new") {
            const char* defName = args.string();
            NodeId nodeId = NodeId(args.int32());
            NodeId targetId = NodeId(args.int32());
            args.drop(); // int32_t addAction = args.int32();

            const std::shared_ptr<SynthDef> def = synthDef(defName);

            auto synthControls = args.atEnd() ? OSCPP::Server::ArgStream() : args.array();
            // FIXME: Cannot be checked before the synth is instantiated.
            // if (def->numControlInputs() != synthControls.size()) {
            //     throw std::runtime_error("Missing synth control initialisers");
            // }
            auto synthArgs = args.atEnd() ? OSCPP::Server::ArgStream() : args.array();

            Node* targetNode = m_nodes.lookup(targetId).get();
            if (targetNode == nullptr)
                throw Error(kMethcla_NodeIdError);
            Group* targetGroup = targetNode->isGroup() ? dynamic_cast<Group*>(targetNode)
                                                       : dynamic_cast<Synth*>(targetNode)->parent();
            Synth* synth = Synth::construct(
                *this,
                nodeId,
                targetGroup,
                Node::kAddToTail,
                *def,
                synthControls,
                synthArgs);
            nodes().insert(synth->id(), synth);

            // Scheduling needed.
            return true;
        } else if (msg == "/query/external_inputs") {
    // Methcla_RequestId requestId = args.int32();
            // sendToWorker(Command(this, perform_response_query_external_inputs, requestId));
        } else if (msg == "/query/external_outputs") {
    // Methcla_RequestId requestId = args.int32();
            // sendToWorker(Command(this, perform_response_query_external_outputs, requestId));
        }
    } catch (Exception& e) {
        // TODO: Reply with error code
        replyError(kMethcla_Notification, e.what());
    } catch (std::exception& e) {
        replyError(kMethcla_Notification, e.what());
    }

    return true;
}

void Environment::processMessage(const OSCPP::Server::Message& msg, Methcla_Time scheduleTime, Methcla_Time currentTime)
{
#if 0
    std::cerr << "Request (recv): " << msg << std::endl;
#endif

    auto args = msg.args();
    // Methcla_RequestId requestId = args.int32();

    try {
        if (msg == "/synth/new") {
            args.drop(); // plugin name
            NodeId nodeId = NodeId(args.int32());
            Node* node = m_nodes.lookup(nodeId).get();
            if (node == nullptr)
                throw Error(kMethcla_NodeIdError);
            else if (!node->isSynth())
                throw Error(kMethcla_NodeTypeError);
            Synth* synth = dynamic_cast<Synth*>(node);
            // TODO: Use sample rate estimate from driver
            const float sampleOffset = (scheduleTime - currentTime) * sampleRate();
            synth->activate(sampleOffset);
        } else if (msg == "/node/free") {
            NodeId nodeId = NodeId(args.int32());

            // Check node id validity
            if (!nodes().contains(nodeId)) {
                throw Error(kMethcla_NodeIdError);
            } else if (nodeId == rootNode()->id()) {
                throw Error(kMethcla_NodeIdError);
            }

            // Drop reference from node map
            m_nodes.remove(nodeId);
        } else if (msg == "/node/set") {
            NodeId nodeId = NodeId(args.int32());
            int32_t index = args.int32();
            float value = args.float32();
            Node* node = m_nodes.lookup(nodeId).get();
            if (node == nullptr)
                throw Error(kMethcla_NodeIdError);
            else if (!node->isSynth())
                throw Error(kMethcla_NodeTypeError);
            Synth* synth = dynamic_cast<Synth*>(node);
            if ((index < 0) || (index >= (int32_t)synth->numControlInputs())) {
                throw std::runtime_error("Control input index out of range");
            }
            synth->controlInput(index) = value;
        } else if (msg == "/synth/map/input") {
            NodeId nodeId = NodeId(args.int32());
            int32_t index = args.int32();
            AudioBusId busId = AudioBusId(args.int32());
            BusMappingFlags flags = BusMappingFlags(args.int32());

            Node* node = m_nodes.lookup(nodeId).get();
            if (node == nullptr)
                throw Error(kMethcla_NodeIdError);
            else if (!node->isSynth())
                throw Error(kMethcla_NodeTypeError);

            Synth* synth = dynamic_cast<Synth*>(node);
            if ((index < 0) || (index >= (int32_t)synth->numAudioInputs()))
                throw std::runtime_error("Synth input index out of range");

            synth->mapInput(index, busId, flags);
        } else if (msg == "/synth/map/output") {
            NodeId nodeId = NodeId(args.int32());
            int32_t index = args.int32();
            AudioBusId busId = AudioBusId(args.int32());
            BusMappingFlags flags = BusMappingFlags(args.int32());

            Node* node = m_nodes.lookup(nodeId).get();
            if (node == nullptr)
                throw Error(kMethcla_NodeIdError);
            else if (!node->isSynth())
                throw Error(kMethcla_NodeTypeError);

            Synth* synth = dynamic_cast<Synth*>(node);
            if ((index < 0) || (index >= (int32_t)synth->numAudioOutputs()))
                throw std::runtime_error("Synth output index out of range");

            synth->mapOutput(index, busId, flags);
        }
    } catch (Exception& e) {
        // TODO: Reply with error code
        replyError(kMethcla_Notification, e.what());
    } catch (std::exception& e) {
        replyError(kMethcla_Notification, e.what());
    }
}

void Environment::registerSynthDef(const Methcla_SynthDef* def)
{
    auto synthDef = std::make_shared<SynthDef>(def);
    m_synthDefs[synthDef->uri()] = synthDef;
}

const std::shared_ptr<SynthDef>& Environment::synthDef(const char* uri) const
{
    auto it = m_synthDefs.find(uri);
    if (it == m_synthDefs.end())
        throw std::runtime_error("Synth definition not found");
    return it->second;
}

void Environment::registerSoundFileAPI(const char* /* mimeType */, const Methcla_SoundFileAPI* api)
{
    m_soundFileAPIs.push_back(api);
}

const Methcla_SoundFileAPI* Environment::soundFileAPI(const char* /* mimeType */) const
{
    return m_soundFileAPIs.empty() ? nullptr : m_soundFileAPIs.front();
}

template <typename T> struct CallbackData
{
    T     func;
    void* arg;
};

static void perform_worldCommand(Environment* env, CommandData* data)
{
    CallbackData<Methcla_WorldPerformFunction>* self = (CallbackData<Methcla_WorldPerformFunction>*)data;
    self->func(env->asWorld(), self->arg);
    env->sendToWorker(perform_nrt_free, self);
}

static void methcla_api_host_perform_command(const Methcla_Host* host, Methcla_WorldPerformFunction perform, void* data)
{
    Environment* env = static_cast<Environment*>(host->handle);
    CallbackData<Methcla_WorldPerformFunction>* callbackData = Methcla::Memory::allocOf<CallbackData<Methcla_WorldPerformFunction>>();
    callbackData->func = perform;
    callbackData->arg = data;
    env->sendFromWorker(perform_worldCommand, callbackData);
}

static void perform_hostCommand(Environment* env, CommandData* data)
{
    CallbackData<Methcla_HostPerformFunction>* self = (CallbackData<Methcla_HostPerformFunction>*)data;
    self->func(env->asHost(), self->arg);
    env->sendFromWorker(perform_rt_free, self);
}

static void methcla_api_world_perform_command(const Methcla_World* world, Methcla_HostPerformFunction perform, void* data)
{
    Environment* env = static_cast<Environment*>(world->handle);
    CallbackData<Methcla_HostPerformFunction>* callbackData = env->rtMem().allocOf<CallbackData<Methcla_HostPerformFunction>>();
    callbackData->func = perform;
    callbackData->arg = data;
    env->sendToWorker(perform_hostCommand, callbackData);
}

#if defined(METHCLA_USE_DUMMY_DRIVER)
# include "Methcla/Audio/IO/DummyDriver.hpp"
#endif

Engine::Engine(PacketHandler handler, IO::Driver::Options driverOptions)
{
#if defined(METHCLA_USE_DUMMY_DRIVER)
    m_driver = new IO::DummyDriver(driverOptions);
#else
    m_driver = IO::defaultPlatformDriver(driverOptions);
#endif
    m_driver->setProcessCallback(processCallback, this);

    std::cout << "Starting methcla engine" << std::endl
              << "  sampleRate = " << m_driver->sampleRate() << std::endl
              << "  numInputs = "  << m_driver->numInputs() << std::endl
              << "  numOutputs = " << m_driver->numOutputs() << std::endl
              << "  bufferSize = " << m_driver->bufferSize() << std::endl;

    Environment::Options options;
    options.sampleRate = m_driver->sampleRate();
    options.blockSize = m_driver->bufferSize();
    options.numHardwareInputChannels = m_driver->numInputs();
    options.numHardwareOutputChannels = m_driver->numOutputs();
    m_env = new Environment(handler, options);
}

Engine::~Engine()
{
    stop();
    delete m_env;
    delete m_driver;
}

void Engine::start()
{
    driver()->start();
}

void Engine::stop()
{
    driver()->stop();
}

void Engine::loadPlugins(const std::list<Methcla_LibraryFunction>& funcs)
{
    m_plugins.loadPlugins(m_env->asHost(), funcs);
}

void Engine::processCallback(
    void* data,
    Methcla_Time currentTime,
    size_t numFrames,
    const sample_t* const* inputs,
    sample_t* const* outputs)
{
    static_cast<Engine*>(data)->m_env->process(currentTime, numFrames, inputs, outputs);
}
