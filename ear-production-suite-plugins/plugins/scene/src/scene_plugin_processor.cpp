#include "scene_plugin_processor.hpp"
#include "scene_plugin_editor.hpp"
#include <adm/write.hpp>
#include "programme_store_adm_serializer.hpp"
#include <future>
#include <programme_store.pb.h>
#include "scene_backend.hpp"
#include "metadata_event_dispatcher.hpp"
#include "pending_store.hpp"
#include "restored_pending_store.hpp"

SceneAudioProcessor::SceneAudioProcessor()
    : AudioProcessor(
      // 64 channels of input supports the largest track width in REAPER.
      // The omission of an output bus is also intentional.
      /// We do not actually manipulate audio here - only analyse it for level.
      BusesProperties().withInput("Input", AudioChannelSet::discreteChannels(64), true)),
      metadata_(std::make_unique<ear::plugin::ui::UIEventDispatcher>(),
                std::make_unique<ear::plugin::MetadataEventDispatcher>(metadataThread_)),
      autoModeController_(std::make_shared<ear::plugin::AutoModeController>(metadata_))
{
  metadata_.addBackendListener(autoModeController_);

  backend_ = std::make_unique<ear::plugin::SceneBackend>(metadata_);

  try {
    backend_->setup();
  } catch (const std::exception& e) {
    backendSetupTimer_ =
      std::make_unique<ear::plugin::BackendSetupTimer>(this);
    backendSetupTimer_->startTimer(1000);
  }

  levelMeter_ =
      std::make_shared<ear::plugin::LevelMeterCalculator>(64, samplerate_);

  samplesSocket = new SamplesSender();
  samplesSocket->open();
  samplesSocket->listen();
  commandSocket = new CommandReceiver(std::bind(
      &SceneAudioProcessor::incomingMessage, this, std::placeholders::_1));
  commandSocket->open();
  commandSocket->listen();

  addParameter(commandPort = new ReadOnlyAudioParameterInt(
                   "commandPort",  // parameter ID
                   "Port that this plug-in is listening on for a COMMAND "
                   "connection",  // parameter name
                   0, 65535,
                   commandSocket->getPort()));  // range and default value

  addParameter(samplesPort = new ReadOnlyAudioParameterInt(
                   "samplesPort",  // parameter ID
                   "Port that this plug-in is listening on for a SAMPLES "
                   "connection",  // parameter name
                   0, 65535,
                   samplesSocket->getPort()));  // range and default value
}

SceneAudioProcessor::~SceneAudioProcessor() {
  sendSamplesToExtension = false;
  backend_.reset();
  commandSocket->close();
  delete commandSocket;
  samplesSocket->close();
  delete samplesSocket;
}

const String SceneAudioProcessor::getName() const { return JucePlugin_Name; }

bool SceneAudioProcessor::acceptsMidi() const {
#if JucePlugin_WantsMidiInput
  return true;
#else
  return false;
#endif
}

bool SceneAudioProcessor::producesMidi() const {
#if JucePlugin_ProducesMidiOutput
  return true;
#else
  return false;
#endif
}

bool SceneAudioProcessor::isMidiEffect() const {
#if JucePlugin_IsMidiEffect
  return true;
#else
  return false;
#endif
}

double SceneAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int SceneAudioProcessor::getNumPrograms() {
  return 1;  // NB: some hosts don't cope very well if you tell them there are 0
             // programs, so this should be at least 1, even if you're not
             // really implementing programs.
}

int SceneAudioProcessor::getCurrentProgram() { return 0; }

void SceneAudioProcessor::setCurrentProgram(int index) {}

const String SceneAudioProcessor::getProgramName(int index) { return {}; }

void SceneAudioProcessor::changeProgramName(int index, const String& newName) {}

void SceneAudioProcessor::prepareToPlay(double sampleRate,
                                        int samplesPerBlock) {
  backend_->triggerMetadataSend();
  doSampleRateChecks();
}

void SceneAudioProcessor::releaseResources() {}

bool SceneAudioProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const {

  // Must accept default config specified in ctor

  if(layouts.inputBuses.size() != 1)
    return false;
  if(layouts.inputBuses[0] != AudioChannelSet::discreteChannels(64))
    return false;

  if(layouts.outputBuses.size() != 0)
    return false;

  return true;
}

void SceneAudioProcessor::processBlock(AudioBuffer<float>& buffer,
                                       MidiBuffer& midiMessages) {

  metadataThread_.post([this](){
    backend_->triggerMetadataSend();
  });
  doSampleRateChecks();

  if(!sendSamplesToExtension) {
    if(getActiveEditor()) {
      levelMeter_->process(buffer);
    }
  } else {
    size_t sampleSize = sizeof(float);
    uint8_t numChannels = 64;
    size_t msg_size = buffer.getNumSamples() * numChannels * sampleSize;

    auto msg = std::make_shared<NngMsg>(msg_size);

    char* msgPosPtr = (char*)msg->getBufferPointer();
    int msgPosOffset = 0;
    float curSample = 0.0;

    int bufferChannels = buffer.getNumChannels();
    int bufferSamplesPerChannel = buffer.getNumSamples();

    for (int sample = 0; sample < bufferSamplesPerChannel; ++sample) {
      for (int channel = 0; channel < numChannels; ++channel) {
        curSample = buffer.getSample(channel, sample);
        assert(msgPosOffset + sampleSize <= msg_size);
        memcpy(msgPosPtr + msgPosOffset, &curSample, sampleSize);
        msgPosOffset += sampleSize;
      }
    }

    auto resSend = samplesSocket->sendBlock(msg, 0);
    assert(resSend == 0);
    if (resSend != 0) {
      // TODO: Log this error somewhere - this means we can't send our samples
      stopExport();
      return;
    }
  }
}

bool SceneAudioProcessor::hasEditor() const {
  return true;  // (change this to false if you choose to not supply an editor)
}

AudioProcessorEditor* SceneAudioProcessor::createEditor() {
  levelMeter_->resetLevels();
  return new SceneAudioProcessorEditor(this);
}

void SceneAudioProcessor::getStateInformation(MemoryBlock& destData) {
  auto [programmes, _] = metadata_.stores();
  destData.setSize(programmes.ByteSizeLong());
  programmes.SerializeToArray(destData.getData(), destData.getSize());
}

void SceneAudioProcessor::setStateInformation(const void* data,
                                              int sizeInBytes) {
  ear::plugin::proto::ProgrammeStore store;
  store.ParseFromArray(data, sizeInBytes);
  restoredStore_ = std::make_shared<ear::plugin::RestoredPendingStore>(metadata_);
  metadata_.addUIListener(restoredStore_);
  // start on the message thread to avoid data race between start and scene updates
  juce::MessageManager::callAsync([restored = std::move(store), this]() {
        restoredStore_->start(restored, metadata_.stores());
  });
}

void SceneAudioProcessor::doSampleRateChecks() {
  auto curSampleRate = static_cast<int>(getSampleRate());
  if (samplerate_ != curSampleRate) {
    samplerate_ = curSampleRate;
    levelMeter_->setup(64, samplerate_);
  }
}

void SceneAudioProcessor::sendAdmMetadata() {
  ear::plugin::ProgrammeStoreAdmSerializer serializer{};
  auto [adm, pluginMaps] =
      serializer.serialize(metadata_.stores());

  std::stringstream ss;
  adm::writeXml(ss, adm);

  std::vector<PluginToAdmMap> pluginToAdmMaps;
  for(int i = 0; i < pluginMaps.size(); ++i) {

    auto atuId = pluginMaps[i].audioTrackUid->get<adm::AudioTrackUidId>();
    auto atuIdNumString = adm::formatId(atuId).substr(4, 8);
    auto atuIdNum = static_cast<uint32_t>(std::stoul(atuIdNumString, nullptr, 16));

    auto aoId = pluginMaps[i].audioObject->get<adm::AudioObjectId>();
    auto aoIdNumString = adm::formatId(aoId).substr(3, 4);
    auto aoIdNum = static_cast<uint32_t>(std::stoul(aoIdNumString, nullptr, 16));

    pluginToAdmMaps.push_back(PluginToAdmMap{aoIdNum, atuIdNum, pluginMaps[i].inputInstanceId, pluginMaps[i].routing});
  }

  commandSocket->sendAdmAndMappings(ss.str(), std::move(pluginToAdmMaps));
}

void SceneAudioProcessor::recvAdmMetadata(std::string admStr,  std::vector<PluginToAdmMap> pluginToAdmMaps) {
    pendingStore_ = std::make_shared<ear::plugin::PendingStore>(metadata_, std::move(admStr), pluginToAdmMaps);
    metadata_.addBackendListener(pendingStore_);
}

void SceneAudioProcessor::incomingMessage(std::shared_ptr<NngMsg> msg) {
  uint8_t cmd;
  memcpy(&cmd, msg->getBufferPointer(), sizeof(uint8_t));

  if (cmd == commandSocket->Command::StartRender) {
    startExport();
    commandSocket->sendResp(cmd);

  } else if (cmd == commandSocket->Command::StopRender) {
    stopExport();
    commandSocket->sendResp(cmd);

  } else if (cmd == commandSocket->Command::GetAdmAndMappings) {
    auto future =
        std::async(std::launch::async, [this]() { sendAdmMetadata(); });
    future.get();

  } else if (cmd == commandSocket->Command::GetConfig) {
    uint8_t numChannels = 64;
    uint32_t sampleRate = samplerate_;
    commandSocket->sendInfo(numChannels, samplerate_);

  } else if (cmd == commandSocket->Command::SetAdmAndMappings) {
    std::vector<PluginToAdmMap> pluginToAdmMaps;
    std::string admStr;
    commandSocket->decodeAdmAndMappingsMessage(msg, admStr, pluginToAdmMaps);

    auto future = std::async(std::launch::async, [this, admStr, pluginToAdmMaps]() {
      recvAdmMetadata(admStr, pluginToAdmMaps);
    });
    future.get();
    commandSocket->sendResp(commandSocket->Command::SetAdmAndMappingsResp);

  } else {
    assert(false);  // If we got here, we received an unsupported command. Just
                    // ignore for now by sending a response anyway.
    commandSocket->sendResp(cmd);
  }
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new SceneAudioProcessor();
}

void SceneAudioProcessor::setupBackend() {
  if (backend_) {
    backend_->setup();
  }
}

ear::plugin::Metadata &SceneAudioProcessor::metadata() {
    return metadata_;
}

void SceneAudioProcessor::startExport() {
    metadata_.setExporting(true);
    sendSamplesToExtension = true;
}

void SceneAudioProcessor::stopExport() {
    metadata_.setExporting(false);
    sendSamplesToExtension = false;
};
