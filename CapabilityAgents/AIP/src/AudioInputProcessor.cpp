/*
 * AudioInputProcessor.cpp
 *
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <sstream>

#include <AVSCommon/Utils/JSON/JSONUtils.h>
#include <AVSCommon/AVS/FocusState.h>
#include <AVSCommon/Utils/Logger/Logger.h>
#include <AVSCommon/Utils/UUIDGeneration/UUIDGeneration.h>

#include "AIP/AudioInputProcessor.h"
#include "AIP/MessageRequest.h"

namespace alexaClientSDK {
namespace capabilityAgents {
namespace aip {

using namespace avsCommon::utils::logger;

/// String to identify log entries originating from this file.
static const std::string TAG("AudioInputProcessor");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

/// The name of the @c FocusManager channel used by @c AudioInputProvider.
static const std::string CHANNEL_NAME = avsCommon::sdkInterfaces::FocusManagerInterface::DIALOG_CHANNEL_NAME;

/// The activityId string used with @c FocusManager by @c AudioInputProvider.
static const std::string ACTIVITY_ID = "SpeechRecognizer.Recognize";

/// The namespace for this capability agent.
static const std::string NAMESPACE = "SpeechRecognizer";

/// The StopCapture directive signature.
static const avsCommon::avs::NamespaceAndName STOP_CAPTURE{NAMESPACE, "StopCapture"};

/// The ExpectSpeech directive signature.
static const avsCommon::avs::NamespaceAndName EXPECT_SPEECH{NAMESPACE, "ExpectSpeech"};

/// The SpeechRecognizer context state signature.
static const avsCommon::avs::NamespaceAndName RECOGNIZER_STATE{NAMESPACE, "RecognizerState"};

std::shared_ptr<AudioInputProcessor> AudioInputProcessor::create(
        std::shared_ptr<avsCommon::sdkInterfaces::DirectiveSequencerInterface> directiveSequencer,
        std::shared_ptr<avsCommon::sdkInterfaces::MessageSenderInterface> messageSender,
        std::shared_ptr<avsCommon::sdkInterfaces::ContextManagerInterface> contextManager,
        std::shared_ptr<avsCommon::sdkInterfaces::FocusManagerInterface> focusManager,
        std::shared_ptr<avsCommon::avs::DialogUXStateAggregator> dialogUXStateAggregator,
        std::shared_ptr<avsCommon::sdkInterfaces::ExceptionEncounteredSenderInterface> exceptionEncounteredSender,
        std::shared_ptr<avsCommon::sdkInterfaces::UserActivityNotifierInterface> userActivityNotifier,
        AudioProvider defaultAudioProvider) {
    if (!directiveSequencer) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullDirectiveSequencer"));
        return nullptr;
    } else if (!messageSender) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullMessageSender"));
        return nullptr;
    } else if (!contextManager) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullContextManager"));
        return nullptr;
    } else if (!focusManager) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullFocusManager"));
        return nullptr;
    } else if (!dialogUXStateAggregator) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullDialogUXStateAggregator"));
        return nullptr;
    } else if (!exceptionEncounteredSender) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullExceptionEncounteredSender"));
        return nullptr;
    } else if (!userActivityNotifier) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullUserActivityNotifier"));
        return nullptr;
    }

    auto aip = std::shared_ptr<AudioInputProcessor>(new AudioInputProcessor(
            directiveSequencer,
            messageSender,
            contextManager,
            focusManager,
            exceptionEncounteredSender,
            userActivityNotifier,
            defaultAudioProvider));

    if (aip) {
        contextManager->setStateProvider(RECOGNIZER_STATE, aip);
        dialogUXStateAggregator->addObserver(aip);
    }

    return aip;
}

avsCommon::avs::DirectiveHandlerConfiguration AudioInputProcessor::getConfiguration() const {
    avsCommon::avs::DirectiveHandlerConfiguration configuration;
    configuration[STOP_CAPTURE] = avsCommon::avs::BlockingPolicy::NON_BLOCKING;
    configuration[EXPECT_SPEECH] = avsCommon::avs::BlockingPolicy::NON_BLOCKING;
    return configuration;
}

void AudioInputProcessor::addObserver(std::shared_ptr<ObserverInterface> observer) {
    if (!observer) {
        ACSDK_ERROR(LX("addObserverFailed").d("reason", "nullObserver"));
        return;
    }
    m_executor.submit([this, observer] () { m_observers.insert(observer); });
}

void AudioInputProcessor::removeObserver(std::shared_ptr<ObserverInterface> observer) {
    if (!observer) {
        ACSDK_ERROR(LX("removeObserverFailed").d("reason", "nullObserver"));
        return;
    }
    m_executor.submit([this, observer] () { m_observers.erase(observer); }).wait();
}

std::future<bool> AudioInputProcessor::recognize(
        AudioProvider audioProvider,
        Initiator initiator,
        avsCommon::avs::AudioInputStream::Index begin,
        avsCommon::avs::AudioInputStream::Index keywordEnd,
        std::string keyword) {
    // If no begin index was provided, grab the current index ASAP so that we can start streaming from the time this
    // call was made.
    if (audioProvider.stream && INVALID_INDEX == begin) {
        static const bool startWithNewData = true;
        auto reader = audioProvider.stream->createReader(
                avsCommon::avs::AudioInputStream::Reader::Policy::NONBLOCKING,
                startWithNewData);
        if (!reader) {
            ACSDK_ERROR(LX("recognizeFailed").d("reason", "createReaderFailed"));
            std::promise<bool> ret;
            ret.set_value(false);
            return ret.get_future();
        }
        begin = reader->tell();
    }
    return m_executor.submit(
        [this, audioProvider, initiator, begin, keywordEnd, keyword] () {
            return executeRecognize(audioProvider, initiator, begin, keywordEnd, keyword);
        }
    );
}

std::future<bool> AudioInputProcessor::stopCapture() {
    return m_executor.submit(
        [this] () {
            return executeStopCapture();
        }
    );
}

std::future<void> AudioInputProcessor::resetState() {
    return m_executor.submit(
        [this] () {
            executeResetState();
        }
    );
}

void AudioInputProcessor::provideState(unsigned int stateRequestToken) {
    m_executor.submit(
        [this, stateRequestToken] () {
            executeProvideState(true, stateRequestToken);
        }
    );
}

void AudioInputProcessor::onContextAvailable(const std::string& jsonContext) {
    m_executor.submit(
        [this, jsonContext] () {
            executeOnContextAvailable(jsonContext);
        }
    );
}

void AudioInputProcessor::onContextFailure(const avsCommon::sdkInterfaces::ContextRequestError error) {
    m_executor.submit(
        [this, error] () {
            executeOnContextFailure(error);
        }
    );
}

void AudioInputProcessor::handleDirectiveImmediately(std::shared_ptr<avsCommon::avs::AVSDirective> directive) {
    handleDirective(std::make_shared<DirectiveInfo>(directive, nullptr));
}

void AudioInputProcessor::preHandleDirective(std::shared_ptr<DirectiveInfo> info) {
}

void AudioInputProcessor::handleDirective(std::shared_ptr<DirectiveInfo> info) {
    if (!info) {
        ACSDK_ERROR(LX("handleDirectiveFailed").d("reason", "nullDirectiveInfo"));
        return;
    }
    if (info->directive->getName() == STOP_CAPTURE.name) {
        handleStopCaptureDirective(info);
    } else if (info->directive->getName() == EXPECT_SPEECH.name) {
        handleExpectSpeechDirective(info);
    } else {
        std::string errorMessage =
                "unexpected directive " +
                info->directive->getNamespace() + ":" + info->directive->getName();
        m_exceptionEncounteredSender->sendExceptionEncountered(
            info->directive->getUnparsedDirective(),
            avsCommon::avs::ExceptionErrorType::UNEXPECTED_INFORMATION_RECEIVED,
            errorMessage);
        if (info->result) {
            info->result->setFailed(errorMessage);
        }
        ACSDK_ERROR(LX("handleDirectiveFailed")
                .d("reason", "unknownDirective")
                .d("namespace", info->directive->getNamespace())
                .d("name", info->directive->getName()));
    }
}

void AudioInputProcessor::cancelDirective(std::shared_ptr<DirectiveInfo> info) {
    resetState();
    removeDirective(info);
}

void AudioInputProcessor::onDeregistered() {
    resetState();
}

void AudioInputProcessor::onFocusChanged(avsCommon::avs::FocusState newFocus) {
    m_executor.submit(
        [this, newFocus] () {
            executeOnFocusChanged(newFocus);
        }
    );
}

void AudioInputProcessor::onDialogUXStateChanged(
        avsCommon::sdkInterfaces::DialogUXStateObserverInterface::DialogUXState newState) {
    m_executor.submit(
        [this, newState] () {
            executeOnDialogUXStateChanged(newState);
        }
    );
}

AudioInputProcessor::AudioInputProcessor(
        std::shared_ptr<avsCommon::sdkInterfaces::DirectiveSequencerInterface> directiveSequencer,
        std::shared_ptr<avsCommon::sdkInterfaces::MessageSenderInterface> messageSender,
        std::shared_ptr<avsCommon::sdkInterfaces::ContextManagerInterface> contextManager,
        std::shared_ptr<avsCommon::sdkInterfaces::FocusManagerInterface> focusManager,
        std::shared_ptr<avsCommon::sdkInterfaces::ExceptionEncounteredSenderInterface> exceptionEncounteredSender,
        std::shared_ptr<avsCommon::sdkInterfaces::UserActivityNotifierInterface> userActivityNotifier,
        AudioProvider defaultAudioProvider) :
        CapabilityAgent{NAMESPACE, exceptionEncounteredSender},
        RequiresShutdown{"AudioInputProcessor"},
        m_directiveSequencer{directiveSequencer},
        m_messageSender{messageSender},
        m_contextManager{contextManager},
        m_focusManager{focusManager},
        m_userActivityNotifier{userActivityNotifier},
        m_defaultAudioProvider{defaultAudioProvider},
        m_lastAudioProvider{AudioProvider::null()},
        m_state{ObserverInterface::State::IDLE},
        m_focusState{avsCommon::avs::FocusState::NONE},
        m_preparingToSend{false},
        m_initialDialogUXStateReceived{false} {
}

void AudioInputProcessor::doShutdown() {
    m_executor.shutdown();
    executeResetState();
    m_directiveSequencer.reset();
    m_messageSender.reset();
    m_contextManager.reset();
    m_focusManager.reset();
    m_userActivityNotifier.reset();
    m_observers.clear();
}

std::future<bool> AudioInputProcessor::expectSpeechTimedOut() {
    return m_executor.submit(
        [this] () {
            return executeExpectSpeechTimedOut();
        }
    );
}

void AudioInputProcessor::handleStopCaptureDirective(std::shared_ptr<DirectiveInfo> info) {
    m_executor.submit(
        [this, info] () {
            bool stopImmediately = true;
            executeStopCapture(stopImmediately, info);
        }
    );
}

void AudioInputProcessor::handleExpectSpeechDirective(std::shared_ptr<DirectiveInfo> info) {
    int64_t timeout;
    bool found = avsCommon::utils::json::jsonUtils::lookupInt64Value(
            info->directive->getPayload(),
            "timeoutInMilliseconds",
            &timeout);

    if (!found) {
        static const char * errorMessage = "missing/invalid timeoutInMilliseconds";
        m_exceptionEncounteredSender->sendExceptionEncountered(
                info->directive->getUnparsedDirective(),
                avsCommon::avs::ExceptionErrorType::UNSUPPORTED_OPERATION,
                errorMessage);
        if (info->result) {
            info->result->setFailed(errorMessage);
        }
        ACSDK_ERROR(LX("handleExpectSpeechDirectiveFailed")
                .d("reason", "missingJsonField")
                .d("field", "timeoutInMilliseconds"));
        return;
    }

    /**
     * TODO: Once AVS starts sending an opaque initiator, parse it out here and pass it on to executeExpectSpeech
     * (ACSDK-479).
     */
    std::string initiator = "";

    m_executor.submit(
        [this, timeout, initiator, info] () {
            executeExpectSpeech(std::chrono::milliseconds{timeout}, initiator, info);
        }
    );
}

bool AudioInputProcessor::executeRecognize(
        AudioProvider provider,
        Initiator initiator,
        avsCommon::avs::AudioInputStream::Index begin,
        avsCommon::avs::AudioInputStream::Index end,
        const std::string& keyword) {
    // Make sure we have a keyword if this is a wakeword initiator.
    if (Initiator::WAKEWORD == initiator && keyword.empty()) {
        ACSDK_ERROR(LX("executeRecognizeFailed").d("reason", "emptyKeywordWithWakewordInitiator"));
        return false;
    }

    // 500ms preroll.
    avsCommon::avs::AudioInputStream::Index preroll = provider.format.sampleRateHz / 2;

    // Check if we have everything we need to enable false wakeword detection.
    // TODO: Consider relaxing the hard requirement for a full 500ms preroll - ACSDK-276.
    bool falseWakewordDetection =
            Initiator::WAKEWORD == initiator &&
            begin != INVALID_INDEX &&
            begin >= preroll &&
            end != INVALID_INDEX;

    // If we will be enabling false wakeword detection, add preroll and build the initiator payload.
    std::ostringstream initiatorPayloadJson;
    // TODO: Consider reworking this code to use RapidJSON - ACSDK-279.
    if (falseWakewordDetection) {
        initiatorPayloadJson
                << R"("wakeWordIndices":{)"
                       R"("startIndexInSamples":)" << preroll << R"(,)"
                       R"("endIndexInSamples":)" << preroll + end - begin
                << R"(})";
        begin -= preroll;
    }

    // Build the initiator json.
    std::ostringstream initiatorJson;
    initiatorJson
            << R"("initiator":{)"
                   R"("type":")" << initiatorToString(initiator) << R"(",)"
                   R"("payload":{)"
            <<         initiatorPayloadJson.str()
            <<     R"(})"
               R"(})";

    return executeRecognize(provider, initiatorJson.str(), begin, keyword);
}

bool AudioInputProcessor::executeRecognize(
        AudioProvider provider,
        const std::string& initiatorJson,
        avsCommon::avs::AudioInputStream::Index begin,
        const std::string& keyword) {
    if (!provider.stream) {
        ACSDK_ERROR(LX("executeRecognizeFailed").d("reason", "nullAudioInputStream"));
        return false;
    }

    if (provider.format.encoding != avsCommon::utils::AudioFormat::Encoding::LPCM) {
        ACSDK_ERROR(LX("executeRecognizeFailed")
                .d("reason", "unsupportedEncoding")
                .d("encoding", provider.format.encoding));
        return false;
    } else if (provider.format.endianness != avsCommon::utils::AudioFormat::Endianness::LITTLE) {
        ACSDK_ERROR(LX("executeRecognizeFailed")
                .d("reason", "unsupportedEndianness")
                .d("endianness", provider.format.endianness));
        return false;
    } else if (provider.format.sampleSizeInBits != 16) {
        ACSDK_ERROR(LX("executeRecognizeFailed")
                .d("reason", "unsupportedSampleSize")
                .d("sampleSize", provider.format.sampleSizeInBits));
        return false;
    } else if (provider.format.sampleRateHz != 16000) {
        ACSDK_ERROR(LX("executeRecognizeFailed")
                .d("reason", "unsupportedSampleRate")
                .d("sampleRate", provider.format.sampleRateHz));
        return false;
    } else if (provider.format.numChannels != 1) {
        ACSDK_ERROR(LX("executeRecognizeFailed")
                .d("reason", "unsupportedNumChannels")
                .d("channels", provider.format.numChannels));
        return false;
    }

    // If this is a barge-in, verify that it is permitted.
    switch (m_state) {
        case ObserverInterface::State::IDLE:
        case ObserverInterface::State::EXPECTING_SPEECH:
            break;
        case ObserverInterface::State::RECOGNIZING:
            // Barge-in is only permitted if the audio providers have compatible policies.
            if (!m_lastAudioProvider.canBeOverridden) {
                ACSDK_ERROR(LX("executeRecognizeFailed").d("reason", "Active audio provider can not be overridden"));
                return false;
            }
            if (!provider.canOverride) {
                ACSDK_ERROR(LX("executeRecognizeFailed").d("reason", "New audio provider can not override"));
                return false;
            }

            // Note: AVS only sends StopCapture for NEAR_FIELD/FAR_FIELD; it does not send StopCapture for CLOSE_TALK.
            if (m_lastAudioProvider && ASRProfile::CLOSE_TALK != m_lastAudioProvider.profile) {
                // TODO: Enable barge-in during recognize once AVS sends dialogRequestId with StopCapture(ACSDK-355).
                ACSDK_ERROR(LX("executeRecognizeFailed").d("reason", "Barge-in is not permitted while recognizing"));
                return false;
            }
            break;
        case ObserverInterface::State::BUSY:
            ACSDK_ERROR(LX("executeRecognizeFailed").d("reason", "Barge-in is not permitted while busy"));
            return false;
    }

    // Assemble the event payload.
    std::ostringstream payload;
    payload << R"({)"
                   R"("profile":")" << provider.profile << R"(",)"
                   R"("format":"AUDIO_L16_RATE_16000_CHANNELS_1")";
    if (!initiatorJson.empty()) {
        payload << "," << initiatorJson;
    }
    payload << R"(})";

    // Set up an attachment reader for the event.
    avsCommon::avs::attachment::InProcessAttachmentReader::SDSTypeIndex offset = 0;
    avsCommon::avs::attachment::InProcessAttachmentReader::SDSTypeReader::Reference reference =
            avsCommon::avs::attachment::InProcessAttachmentReader::SDSTypeReader::Reference::BEFORE_WRITER;
    if (INVALID_INDEX != begin) {
        offset = begin;
        reference = avsCommon::avs::attachment::InProcessAttachmentReader::SDSTypeReader::Reference::ABSOLUTE;
    }
    m_reader = avsCommon::avs::attachment::InProcessAttachmentReader::create(
            avsCommon::avs::attachment::AttachmentReader::Policy::NON_BLOCKING,
            provider.stream,
            offset,
            reference);
    if (!m_reader) {
        ACSDK_ERROR(LX("executeRecognizeFailed").d("reason", "Failed to create attachment reader"));
        return false;
    }

    // Code below this point changes the state of AIP.  Formally update state now, and don't error out without calling
    // executeResetState() after this point.
    setState(ObserverInterface::State::RECOGNIZING);

    // Note that we're preparing to send a Recognize event.
    m_preparingToSend = true;

    //  Start assembling the context; we'll service the callback after assembling our Recognize event.
    m_contextManager->getContext(shared_from_this());

    // Stop the ExpectSpeech timer so we don't get a timeout.
    m_expectingSpeechTimer.stop();

    // Update state if we're changing wakewords.
    if (!keyword.empty() && m_wakeword != keyword) {
        m_wakeword = keyword;
        executeProvideState();
    }

    // Record provider as the last-used AudioProvider so it can be used in the event of an ExpectSpeech directive.
    m_lastAudioProvider = provider;

    // Record the event payload for later use by executeContextAvailable().
    m_payload = payload.str();

    // We can't assemble the MessageRequest until we receive the context.
    m_request.reset();

    return true;
}

void AudioInputProcessor::executeOnContextAvailable(const std::string jsonContext) {
    ACSDK_DEBUG(LX("executeOnContextAvailable")
            .d("jsonContext", jsonContext));

    // Should already be RECOGNIZING if we get here.
    if (m_state != ObserverInterface::State::RECOGNIZING) {
        ACSDK_ERROR(LX("executeOnContextAvailableFailed")
                .d("reason", "Not permitted in current state")
                .d("state", m_state));
       return;
    }

    // Should already have a reader.
    if (!m_reader) {
        ACSDK_ERROR(LX("executeOnContextAvailableFailed").d("reason", "nullReader"));
        executeResetState();
        return;
    }
    // Payload should not be empty.
    if (m_payload.empty()) {
        ACSDK_ERROR(LX("executeOnContextAvailableFailed").d("reason", "payloadEmpty"));
        executeResetState();
        return;
    }

    // Start acquiring the channel right away; we'll service the callback after assembling our Recognize event.
    if (m_focusState != avsCommon::avs::FocusState::FOREGROUND) {
        if (!m_focusManager->acquireChannel(CHANNEL_NAME, shared_from_this(), ACTIVITY_ID)) {
            ACSDK_ERROR(LX("executeOnContextAvailableFailed").d("reason", "Unable to acquire channel"));
            executeResetState();
            return;
        }
    }

    // Assemble the MessageRequest.  It will be sent by executeOnFocusChanged when we acquire the channel.
    auto dialogRequestId = avsCommon::utils::uuidGeneration::generateUUID();
    m_directiveSequencer->setDialogRequestId(dialogRequestId);
    auto msgIdAndJsonEvent = buildJsonEventString(
            "Recognize",
            dialogRequestId,
            m_payload,
            jsonContext);
    m_request = std::make_shared<MessageRequest>(shared_from_this(), msgIdAndJsonEvent.second, m_reader);

    // If we already have focus, there won't be a callback to send the message, so send it now.
    if (avsCommon::avs::FocusState::FOREGROUND == m_focusState) {
        sendRequestNow();
    }
}

void AudioInputProcessor::executeOnContextFailure(const avsCommon::sdkInterfaces::ContextRequestError error) {
    ACSDK_ERROR(LX("executeOnContextFailure").d("error", error));
    executeResetState();
}

void AudioInputProcessor::executeOnFocusChanged(avsCommon::avs::FocusState newFocus) {
    ACSDK_DEBUG(LX("executeOnFocusChanged").d("newFocus", newFocus));

    // Note new focus state.
    m_focusState = newFocus;

    // If we're losing focus, stop using the channel.
    if (newFocus != avsCommon::avs::FocusState::FOREGROUND) {
        ACSDK_DEBUG(LX("executeOnFocusChanged").d("reason", "Lost focus"));
        executeResetState();
        return;
    }

    // We're not losing the channel (m_focusState == avsCommon::avs::FocusState::FOREGROUND).  For all
    // states except RECOGNIZING, there's nothing more to do here.
    if (m_state != ObserverInterface::State::RECOGNIZING) {
        return;
    }

    // For a focus change to FOREGROUND in the Recognizing state, we may have a message queued up to send.  If we do,
    // we can safely send it now.
    if (m_request) {
        sendRequestNow();
    }
}

bool AudioInputProcessor::executeStopCapture(bool stopImmediately, std::shared_ptr<DirectiveInfo> info) {
    if (m_state != ObserverInterface::State::RECOGNIZING) {
        static const char * errorMessage = "StopCapture only allowed in RECOGNIZING state.";
        if (info) {
            if (info->result) {
                info->result->setFailed(errorMessage);
            }
            removeDirective(info);
        }
        ACSDK_ERROR(LX("executeStopCaptureFailed")
                .d("reason", "invalidState")
                .d("expectedState", "RECOGNIZING")
                .d("state", m_state));
        return false;
    }
    // Create a lambda to do the StopCapture.
    std::function<void()> stopCapture = [=] {
        ACSDK_DEBUG(LX("stopCapture").d("stopImmediately", stopImmediately));
        if (stopImmediately) {
            m_reader->close(avsCommon::avs::attachment::AttachmentReader::ClosePoint::IMMEDIATELY);
        } else {
            m_reader->close(avsCommon::avs::attachment::AttachmentReader::ClosePoint::AFTER_DRAINING_CURRENT_BUFFER);
        }

        m_reader.reset();
        setState(ObserverInterface::State::BUSY);

        if (info) {
            if (info->result) {
                info->result->setCompleted();
            }
            removeDirective(info);
        }
    };

    if (m_preparingToSend) {
        // If we're still preparing to send, we'll save the lambda and call it after we start sending.
        m_deferredStopCapture = stopCapture;
    } else {
        // Otherwise, we can call the lambda now.
        stopCapture();
    }

    return true;
}

void AudioInputProcessor::executeResetState() {
    // Irrespective of current state, clean up and go back to idle.
    m_expectingSpeechTimer.stop();
    if (m_reader) {
        m_reader->close();
    }
    m_reader.reset();
    m_request.reset();
    m_preparingToSend = false;
    m_deferredStopCapture = nullptr;
    if (m_focusState != avsCommon::avs::FocusState::NONE) {
        m_focusManager->releaseChannel(CHANNEL_NAME, shared_from_this());
    }
    m_focusState = avsCommon::avs::FocusState::NONE;
    setState(ObserverInterface::State::IDLE);
}

bool AudioInputProcessor::executeExpectSpeech(
        std::chrono::milliseconds timeout, std::string initiator, std::shared_ptr<DirectiveInfo> info) {

    if (m_state != ObserverInterface::State::IDLE && 
                m_state != ObserverInterface::State::BUSY) {
        static const char * errorMessage = "ExpectSpeech only allowed in IDLE or BUSY state.";
        if (info->result) {
            info->result->setFailed(errorMessage);
        }
        removeDirective(info);
        ACSDK_ERROR(LX("executeExpectSpeechFailed")
                .d("reason", "invalidState")
                .d("expectedState", "IDLE/BUSY")
                .d("state", m_state));
        return false;
    }

    // Start the ExpectSpeech timer.
    if (!m_expectingSpeechTimer.start(timeout, std::bind(&AudioInputProcessor::expectSpeechTimedOut, this)).valid()) {
        ACSDK_ERROR(LX("executeExpectSpeechFailed").d("reason", "startTimerFailed"));
    }
    setState(ObserverInterface::State::EXPECTING_SPEECH);
    if (info->result) {
        info->result->setCompleted();
    }
    removeDirective(info);

    // If possible, start recognizing immediately.
    if (m_lastAudioProvider && m_lastAudioProvider.alwaysReadable) {
        return executeRecognize(m_lastAudioProvider, initiator);
    } else if (m_defaultAudioProvider && m_defaultAudioProvider.alwaysReadable) {
        return executeRecognize(m_defaultAudioProvider, initiator);
    }

    return true;
}

bool AudioInputProcessor::executeExpectSpeechTimedOut() {
    if (m_state != ObserverInterface::State::EXPECTING_SPEECH) {
        ACSDK_ERROR(LX("executeExpectSpeechTimedOutFailure")
                .d("reason", "invalidState")
                .d("expectedState", "EXPECTING_SPEECH")
                .d("state", m_state));
        return false;
    }

    auto msgIdAndJsonEvent = buildJsonEventString("ExpectSpeechTimedOut");
    auto request = std::make_shared<MessageRequest>(shared_from_this(), msgIdAndJsonEvent.second, m_reader);
    m_messageSender->sendMessage(request);
    setState(ObserverInterface::State::IDLE);
    ACSDK_ERROR(LX("executeExpectSpeechFailed").d("reason", "Timed Out"));
    return true;
}

void AudioInputProcessor::executeProvideState(bool sendToken, unsigned int stateRequestToken) {
    std::ostringstream context;
    context << R"({"wakeword" : ")" << m_wakeword << R"("})";
    if (sendToken) {
        m_contextManager->setState(
                RECOGNIZER_STATE,
                context.str(),
                avsCommon::avs::StateRefreshPolicy::NEVER,
                stateRequestToken);
    } else {
        m_contextManager->setState(
                RECOGNIZER_STATE,
                context.str(),
                avsCommon::avs::StateRefreshPolicy::NEVER);
    }
}

void AudioInputProcessor::executeOnDialogUXStateChanged(
        avsCommon::sdkInterfaces::DialogUXStateObserverInterface::DialogUXState newState) {
    if (!m_initialDialogUXStateReceived) {
        // The initial dialog UX state change call comes from simply registering as an observer; it is not a deliberate
        // change to the dialog state which should interrupt a recognize event.
        m_initialDialogUXStateReceived = true;
        return;
    }
    if (newState != avsCommon::sdkInterfaces::DialogUXStateObserverInterface::DialogUXState::IDLE) {
        return;
    }
    if (m_focusState != avsCommon::avs::FocusState::NONE) {
        m_focusManager->releaseChannel(CHANNEL_NAME, shared_from_this());
        m_focusState = avsCommon::avs::FocusState::NONE;
    }
    setState(ObserverInterface::State::IDLE);
}

void AudioInputProcessor::setState(ObserverInterface::State state) {
    if (m_state == state) {
        return;
    }

    // Reset the user inactivity if transitioning to or from `RECOGNIZING` state.
    if (ObserverInterface::State::RECOGNIZING == m_state || ObserverInterface::State::RECOGNIZING == state) {
        m_userActivityNotifier->onUserActive();
    }

    ACSDK_DEBUG(LX("setState").d("from", m_state).d("to", state));
    m_state = state;
    for (auto observer: m_observers) {
        observer->onStateChanged(m_state);
    }
}

void AudioInputProcessor::removeDirective(std::shared_ptr<DirectiveInfo> info) {
    // Check result too, to catch cases where DirectiveInfo was created locally, without a nullptr result.
    // In those cases there is no messageId to remove because no result was expected.
    if (info->directive && info->result) {
        CapabilityAgent::removeDirective(info->directive->getMessageId());
    }
}

void AudioInputProcessor::sendRequestNow() {
    m_messageSender->sendMessage(m_request);
    m_request.reset();
    m_preparingToSend = false;
    if (m_deferredStopCapture) {
        m_deferredStopCapture();
        m_deferredStopCapture = nullptr;
    }
}

} // namespace aip
} // namespace capabilityAgents
} // namespace alexaClientSDK
