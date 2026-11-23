#include "UERLBridgeServer.h"

#include "UERLBatchLayout.h"
#include "UERLJson.h"
#include "UERLProtocol.h"
#include "UERLSocketBridgeCodec.h"
#include "UERLTransportAdapter.h"
#include "UERLTransportLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/FileManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/Archive.h"

namespace UERLSocketBridgeServerPrivate
{
	using namespace UERLProtocol;
	using namespace UERLBatchLayout;
	using namespace UERLSocketBridgeCodec;

	enum class ESessionState : uint8
	{
		Connected,
		Negotiated,
		WaitingReady,
		Ready,
		Closing,
		Failed,
	};

	const TCHAR* StateName(ESessionState State)
	{
		switch (State)
		{
		case ESessionState::Connected: return TEXT("connected");
		case ESessionState::Negotiated: return TEXT("negotiated");
		case ESessionState::WaitingReady: return TEXT("waiting_ready_ack");
		case ESessionState::Ready: return TEXT("ready");
		case ESessionState::Closing: return TEXT("closing");
		default: return TEXT("failed");
		}
	}
}

using namespace UERLSocketBridgeServerPrivate;

class FUERLBridgeServer::FImpl final : public FRunnable
{
public:
	~FImpl() override { Shutdown(); }

	bool Start(
		const FUERLBridgeServerConfig& InConfig,
		const FUERLBridgeCapabilities& InCapabilities,
		IBridgeRequestHandler& InHandler,
		FString& OutError)
	{
		if (Thread) { OutError = TEXT("SocketBridge is already running"); return false; }
		if (!InConfig.IsValid()) { OutError = TEXT("invalid SocketBridge bootstrap configuration"); return false; }
		Config = InConfig;
		Capabilities = InCapabilities;
		Handler = &InHandler;
		State = ESessionState::Connected;
		Session = FGuid();
		ExpectedSequence = 1;
		StepCount = 0;
		SelectedSchema = FUERLBatchSchema();
		bDescribePrepared = false;
		PreparedProjection = FUERLWorkerProjection();
		PreparedActionFields.Reset();
		PreparedStateFields.Reset();
		PreparedTopology = FUERLRobotTopology();
		Layouts = FSet();
		Actions.Reset();
		States.Reset();
		Faults.Reset();
		Episodes.Reset();
		if (!Config.PerformancePath.IsEmpty())
		{
			PerformanceLog.Reset(IFileManager::Get().CreateFileWriter(
				*Config.PerformancePath, FILEWRITE_Append));
			if (!PerformanceLog)
			{
				OutError = FString::Printf(TEXT("failed to open performance log '%s'"), *Config.PerformancePath);
				return false;
			}
		}
		Transport = MakeUERLSocketTransport();
		if (!Transport || !Transport->Listen(Config.Port, OutError)) { CloseTransport(); return false; }
		UE_LOG(LogUERLTransport, Display, TEXT("[FLOW] SocketBridge listening 127.0.0.1:%d"), Config.Port);
		bStopRequested = false;
		bGracefulShutdown = false;
		bTerminalErrorResponseSent = false;
		Thread = FRunnableThread::Create(this, TEXT("UERLSocketBridge"), 0, TPri_AboveNormal);
		if (!Thread) { OutError = TEXT("failed to create SocketBridge IO thread"); CloseTransport(); return false; }
		return true;
	}

	virtual void Stop() override { RequestStop(); }

	void Shutdown()
	{
		RequestStop();
		if (Thread)
		{
			FRunnableThread* Completed = Thread;
			Thread = nullptr;
			Completed->WaitForCompletion();
			delete Completed;
		}
		CloseTransport();
		if (PerformanceLog)
		{
			PerformanceLog->Flush();
			PerformanceLog.Reset();
		}
		Handler = nullptr;
	}

	bool IsRunning() const { return Thread != nullptr && !bStopRequested; }

	virtual uint32 Run() override
	{
		FString AcceptError;
		if (!Transport || !Transport->Accept(Config.HandshakeTimeoutMs, bStopRequested, AcceptError))
		{
			if (!bStopRequested) { UE_LOG(LogUERLTransport, Error, TEXT("[VERIFY] SocketBridge accept failed: %s"), *AcceptError); }
			CloseTransport();
			if (!bStopRequested && Handler) { Handler->NotifyBridgeDisconnected(); }
			return 1;
		}
		UE_LOG(LogUERLTransport, Display, TEXT("[FLOW] SocketBridge client connected"));

		while (!bStopRequested && State != ESessionState::Failed && !bGracefulShutdown)
		{
			FFrameHeader Header;
			TArray<uint8> Payload;
			FString Error;
			if (!ReceiveFrame(Header, Payload, Error))
			{
				if (!bStopRequested) { UE_LOG(LogUERLTransport, Error, TEXT("[VERIFY] SocketBridge receive failed: %s"), *Error); }
				break;
			}
			if (!ProcessFrame(Header, Payload)) { break; }
		}

		if (bTerminalErrorResponseSent && !bStopRequested && Transport)
		{
			Transport->WaitForPeerClose(FMath::Min(Config.RequestTimeoutMs, 100u), bStopRequested);
		}
		CloseTransport();
		if (Handler)
		{
			if (bGracefulShutdown) { Handler->NotifyBridgeShutdownComplete(); }
			else { Handler->NotifyBridgeDisconnected(); }
		}
		return bGracefulShutdown || bStopRequested ? 0 : 1;
	}

private:
	void RequestStop()
	{
		bStopRequested = true;
		if (Transport) { Transport->Interrupt(); }
	}

	void CloseTransport()
	{
		if (Transport) { Transport->Close(); Transport.Reset(); }
	}

	bool ReceiveFrame(FFrameHeader& OutHeader, TArray<uint8>& OutPayload, FString& OutError)
	{
		uint8 HeaderBytes[FrameHeaderSize];
		const uint32 Timeout = State == ESessionState::Connected ? Config.HandshakeTimeoutMs : Config.RequestTimeoutMs;
		ActiveDeadlineSeconds = FPlatformTime::Seconds() + Timeout / 1000.0;
		if (!Transport || !Transport->ReadExact(HeaderBytes, FrameHeaderSize, ActiveDeadlineSeconds, bStopRequested))
		{
			OutError = TEXT("deadline, disconnect, or partial header");
			return false;
		}
		if (!FFrameHeader::Decode(HeaderBytes, OutHeader, OutError)) { return false; }
		if (State == ESessionState::Connected && IsUuidV4(OutHeader.Session)) { Session = OutHeader.Session; }
		FString Code;
		if (!ValidateHeaderBeforePayload(OutHeader, Code, OutError))
		{
			Fail(Code, OutError, OutHeader.Sequence);
			return false;
		}
		OutPayload.SetNumUninitialized(OutHeader.PayloadLength);
		if (OutHeader.PayloadLength > 0
			&& (!Transport || !Transport->ReadExact(
				OutPayload.GetData(), OutPayload.Num(), ActiveDeadlineSeconds, bStopRequested)))
		{
			OutError = TEXT("deadline, disconnect, or partial payload");
			return false;
		}
		return true;
	}

	bool ValidateHeaderBeforePayload(const FFrameHeader& Header, FString& OutCode, FString& OutError) const
	{
		EMessage Expected = EMessage::Hello;
		uint64 ExpectedLayout = 0;
		switch (State)
		{
		case ESessionState::Connected: Expected = EMessage::Hello; break;
		case ESessionState::Negotiated:
			Expected = Header.Message == EMessage::Shutdown ? EMessage::Shutdown : EMessage::Initialize;
			break;
		case ESessionState::WaitingReady:
			Expected = Header.Message == EMessage::Shutdown ? EMessage::Shutdown : EMessage::Ready;
			break;
		case ESessionState::Ready:
			Expected = Header.Message;
			if (Header.Message == EMessage::Step) { ExpectedLayout = Layouts.StepAction.LayoutId; }
			else if (Header.Message == EMessage::Reset) { ExpectedLayout = Layouts.ResetRequest.LayoutId; }
			else if (Header.Message != EMessage::Shutdown && Header.Message != EMessage::Event)
			{
				OutCode = TEXT("STATE_VIOLATION");
				OutError = TEXT("message is not legal in the Ready state");
				return false;
			}
			break;
		default:
			OutCode = TEXT("STATE_VIOLATION");
			OutError = TEXT("Session does not accept another request");
			return false;
		}
		if (!ValidateEnvelope(Header, Expected, ExpectedLayout, OutCode, OutError)) { return false; }
		if (Header.Message == EMessage::Step && Header.PayloadLength != Layouts.StepAction.PayloadLength)
		{
			OutCode = TEXT("PAYLOAD_INVALID");
			OutError = TEXT("Step payload length mismatch");
			return false;
		}
		if (Header.Message == EMessage::Reset && Header.PayloadLength != Layouts.ResetRequest.PayloadLength)
		{
			OutCode = TEXT("PAYLOAD_INVALID");
			OutError = TEXT("Reset payload length mismatch");
			return false;
		}
		return true;
	}

	bool SendFrame(EMessage Message, uint16 Flags, uint64 Sequence, uint64 LayoutId, const TArray<uint8>& Payload)
	{
		FFrameHeader Header;
		Header.ProtocolMajor = State == ESessionState::Connected ? 0 : Major;
		Header.ProtocolMinor = State == ESessionState::Connected ? 0 : Minor;
		Header.Message = Message;
		Header.Flags = Flags;
		Header.PayloadLength = Payload.Num();
		Header.Sequence = Sequence;
		Header.Session = Session;
		Header.LayoutId = LayoutId;
		uint8 HeaderBytes[FrameHeaderSize];
		Header.Encode(HeaderBytes);
		return Transport
			&& Transport->WriteExact(HeaderBytes, FrameHeaderSize, ActiveDeadlineSeconds, bStopRequested)
			&& (Payload.IsEmpty() || Transport->WriteExact(
				Payload.GetData(), Payload.Num(), ActiveDeadlineSeconds, bStopRequested));
	}

	uint32 RemainingTimeoutMs() const
	{
		const double RemainingSeconds = ActiveDeadlineSeconds - FPlatformTime::Seconds();
		if (RemainingSeconds <= 0.0) { return 0; }
		return static_cast<uint32>(FMath::Clamp(
			FMath::CeilToDouble(RemainingSeconds * 1000.0), 1.0, static_cast<double>(MAX_uint32)));
	}

	void SubmitWorker(FUERLBridgeRequest& Request) const
	{
		Request.TimeoutMs = RemainingTimeoutMs();
		if (Request.TimeoutMs == 0)
		{
			Request.Fail(UERLBridgeError::TransportFailure, TEXT("request transaction deadline expired"));
			return;
		}
		Handler->SubmitAndWait(Request);
	}

	bool Fail(const FString& Code, const FString& Message, uint64 Sequence)
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Code);
		Error->SetStringField(TEXT("phase"), StateName(State));
		Error->SetStringField(TEXT("message"), Message.Left(512));
		Error->SetObjectField(TEXT("details"), MakeShared<FJsonObject>());
		bTerminalErrorResponseSent = SendFrame(
			EMessage::Error, Response | UERLProtocol::Error, Sequence, 0, SerializeJson(Error));
		State = ESessionState::Failed;
		UE_LOG(LogUERLTransport, Error, TEXT("[VERIFY] SocketBridge %s: %s"), *Code, *Message);
		return false;
	}

	bool ValidateEnvelope(const FFrameHeader& Header, EMessage Expected, uint64 ExpectedLayout,
		FString& OutCode, FString& OutError) const
	{
		if (Header.Flags != Request || Header.Message != Expected)
		{
			OutCode = TEXT("STATE_VIOLATION");
			OutError = TEXT("message is not legal in the current protocol state");
			return false;
		}
		if (Header.Sequence != ExpectedSequence)
		{
			OutCode = TEXT("SEQUENCE_MISMATCH");
			OutError = FString::Printf(TEXT("sequence mismatch got=%llu expected=%llu"), Header.Sequence, ExpectedSequence);
			return false;
		}
		if (State == ESessionState::Connected)
		{
			if (Header.ProtocolMajor != 0 || Header.ProtocolMinor != 0)
			{
				OutCode = TEXT("VERSION_UNSUPPORTED");
				OutError = TEXT("Hello requires bootstrap protocol version 0.0");
				return false;
			}
			if (!IsUuidV4(Header.Session))
			{
				OutCode = TEXT("SESSION_MISMATCH");
				OutError = TEXT("Hello requires the client Session UUID v4");
				return false;
			}
		}
		else
		{
			if (Header.ProtocolMajor != Major || Header.ProtocolMinor != Minor)
			{
				OutCode = TEXT("VERSION_UNSUPPORTED");
				OutError = TEXT("protocol version mismatch");
				return false;
			}
			if (Header.Session != Session)
			{
				OutCode = TEXT("SESSION_MISMATCH");
				OutError = TEXT("Session UUID mismatch");
				return false;
			}
		}
		if (Header.LayoutId != ExpectedLayout)
		{
			OutCode = TEXT("PAYLOAD_INVALID");
			OutError = TEXT("layout_id mismatch");
			return false;
		}
		return true;
	}

	bool ProcessFrame(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		if (State == ESessionState::Connected)
		{
			Session = Header.Session;
			return ProcessHello(Header, Payload);
		}
		if (State == ESessionState::Negotiated)
		{
			if (Header.Message == EMessage::Shutdown) { return ProcessShutdown(Header, Payload); }
			return ProcessInitialize(Header, Payload);
		}
		if (State == ESessionState::WaitingReady)
		{
			if (Header.Message == EMessage::Shutdown) { return ProcessShutdown(Header, Payload); }
			return ProcessReady(Header, Payload);
		}
		if (State == ESessionState::Ready)
		{
			if (Header.Message == EMessage::Step) { return ProcessStep(Header, Payload); }
			if (Header.Message == EMessage::Reset) { return ProcessReset(Header, Payload); }
			if (Header.Message == EMessage::Event) { return ProcessEvent(Header, Payload); }
			if (Header.Message == EMessage::Shutdown) { return ProcessShutdown(Header, Payload); }
		}
		return Fail(TEXT("STATE_VIOLATION"), TEXT("message is not legal in the current protocol state"), Header.Sequence);
	}

	bool ProcessHello(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Hello, 0, Code, Error)) { return Fail(Code, Error, Header.Sequence); }
		TSharedPtr<FJsonObject> Json;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !CheckKeys(Json, { TEXT("supported_protocols"), TEXT("client_name"), TEXT("client_version") }, { TEXT("extensions") }, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		const TArray<TSharedPtr<FJsonValue>>* Protocols = nullptr;
		FString ClientName;
		FString ClientVersion;
		if (!Json->TryGetArrayField(TEXT("supported_protocols"), Protocols) || !Protocols
			|| !Json->TryGetStringField(TEXT("client_name"), ClientName) || ClientName.IsEmpty()
			|| !Json->TryGetStringField(TEXT("client_version"), ClientVersion) || ClientVersion.IsEmpty())
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Hello DTO types are invalid"), Header.Sequence);
		}
		bool bSupported = false;
		for (const TSharedPtr<FJsonValue>& Value : *Protocols)
		{
			const TSharedPtr<FJsonObject> Version = Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject() : nullptr;
			int32 RequestedMajor = 0, MinMinor = 0, MaxMinor = 0;
			if (!CheckKeys(Version, { TEXT("major"), TEXT("min_minor"), TEXT("max_minor") }, {}, Error)
				|| !ReadInt(Version, TEXT("major"), 0, 65535, RequestedMajor, Error)
				|| !ReadInt(Version, TEXT("min_minor"), 0, 65535, MinMinor, Error)
				|| !ReadInt(Version, TEXT("max_minor"), 0, 65535, MaxMinor, Error))
			{
				return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
			}
			bSupported |= RequestedMajor == Major && MinMinor <= Minor && Minor <= MaxMinor;
		}
		if (!bSupported) { return Fail(TEXT("VERSION_UNSUPPORTED"), TEXT("no common protocol version"), Header.Sequence); }

		State = ESessionState::Negotiated;
		++ExpectedSequence;
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetObjectField(TEXT("selected_protocol"), ProtocolVersionJson());
		ResponseJson->SetObjectField(TEXT("build_identity"), BuildIdentityJson());
		TSharedRef<FJsonObject> CapabilitiesJson = MakeShared<FJsonObject>();
		CapabilitiesJson->SetBoolField(TEXT("socket_data_plane"), true);
		CapabilitiesJson->SetBoolField(TEXT("headless"), Capabilities.bHeadless);
		CapabilitiesJson->SetBoolField(TEXT("viewport"), Capabilities.bViewport);
		CapabilitiesJson->SetStringField(TEXT("presentation_mode"), Capabilities.ActivePresentationMode);
		ResponseJson->SetObjectField(TEXT("capabilities"), CapabilitiesJson);
		TSharedRef<FJsonObject> Limits = MakeShared<FJsonObject>();
		Limits->SetNumberField(TEXT("max_control_payload_bytes"), MaxControlPayloadBytes);
		Limits->SetNumberField(TEXT("max_data_payload_bytes"), MaxDataPayloadBytes);
		Limits->SetNumberField(TEXT("max_slots"), 65536);
		ResponseJson->SetObjectField(TEXT("limits"), Limits);
		return SendFrame(EMessage::Hello, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
	}

	bool ProcessInitialize(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Initialize, 0, Code, Error)) { return Fail(Code, Error, Header.Sequence); }
		TSharedPtr<FJsonObject> Json;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !CheckKeys(Json,
				{ TEXT("task_id"), TEXT("task_version"), TEXT("worker_config"), TEXT("worker_config_hash"),
				  TEXT("state_requirements"), TEXT("action_schema"), TEXT("phase") }, { TEXT("extensions") }, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		FString TaskId, TaskVersion, ConfigHash, Phase;
		const TSharedPtr<FJsonObject>* WorkerConfig = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* StateRequirements = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ActionSchema = nullptr;
		if (!Json->TryGetStringField(TEXT("task_id"), TaskId) || TaskId.IsEmpty()
			|| !Json->TryGetStringField(TEXT("task_version"), TaskVersion) || TaskVersion.IsEmpty()
			|| !Json->TryGetObjectField(TEXT("worker_config"), WorkerConfig) || !WorkerConfig
			|| !Json->TryGetStringField(TEXT("worker_config_hash"), ConfigHash) || !IsHex32(ConfigHash)
			|| !Json->TryGetArrayField(TEXT("state_requirements"), StateRequirements) || !StateRequirements
			|| !Json->TryGetArrayField(TEXT("action_schema"), ActionSchema) || !ActionSchema
			|| !Json->TryGetStringField(TEXT("phase"), Phase))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Initialize DTO types are invalid"), Header.Sequence);
		}
		if (Phase != TEXT("describe") && Phase != TEXT("commit"))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Initialize phase must be describe or commit"), Header.Sequence);
		}
		const bool bDescribe = Phase == TEXT("describe");
		FString ComputedHash;
		if (!UERLJson::CanonicalSha256(*WorkerConfig, ComputedHash, Error) || ComputedHash != ConfigHash)
		{
			return Fail(TEXT("CONFIG_REJECTED"), TEXT("worker_config_hash mismatch"), Header.Sequence);
		}

		FUERLWorkerProjection Projection;
		TArray<FUERLFieldDescriptor> RequestedStates;
		TArray<FUERLFieldDescriptor> RequestedActions;
		if (!ParseWorkerConfig(*WorkerConfig, Projection, Error)) { return Fail(TEXT("CONFIG_REJECTED"), Error, Header.Sequence); }
		if (!bDescribe && (!ParseFields(*StateRequirements, RequestedStates, false, Error)
			|| !ParseFields(*ActionSchema, RequestedActions, true, Error)))
		{
			return Fail(TEXT("SCHEMA_MISMATCH"), Error, Header.Sequence);
		}

		FUERLBridgeRequest Prepare;
		if (bDescribe)
		{
			if (bDescribePrepared)
			{
				return Fail(TEXT("STATE_VIOLATION"), TEXT("Initialize describe was already completed"), Header.Sequence);
			}
			Prepare.Type = EUERLBridgeRequestType::PrepareInitialize;
			Prepare.Sequence = Header.Sequence;
			Prepare.Projection = Projection;
			SubmitWorker(Prepare);
			if (!Prepare.bOk) { return Fail(WorkerErrorCode(Prepare.ErrorCode), Prepare.ErrorMessage, Header.Sequence); }
			bDescribePrepared = true;
			PreparedProjection = Prepare.Projection;
			if (!WorkerIdentityHash(PreparedProjection, PreparedIdentityHash, Error))
			{
				return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
			}
			PreparedActionFields = Prepare.AvailableActionFields;
			PreparedStateFields = Prepare.AvailableStateFields;
			PreparedTopology = Prepare.AvailableTopology;
			TSharedRef<FJsonObject> EffectiveConfig = WorkerConfigJson(Prepare.Projection);
			FString EffectiveHash;
			if (!UERLJson::CanonicalSha256(EffectiveConfig, EffectiveHash, Error))
			{
				return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
			}
			TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
			ResponseJson->SetStringField(TEXT("phase"), TEXT("describe"));
			ResponseJson->SetObjectField(TEXT("selected_protocol"), ProtocolVersionJson());
			ResponseJson->SetObjectField(TEXT("build_identity"), BuildIdentityJson());
			ResponseJson->SetObjectField(TEXT("effective_worker_config"), EffectiveConfig);
			ResponseJson->SetStringField(TEXT("effective_worker_config_hash"), EffectiveHash);
			ResponseJson->SetField(TEXT("available_state_schema"), DescriptorArray(PreparedStateFields));
			ResponseJson->SetField(TEXT("available_action_schema"), DescriptorArray(PreparedActionFields));
			ResponseJson->SetObjectField(TEXT("topology"), TopologyJson(PreparedTopology));
			ResponseJson->SetStringField(TEXT("seed_derivation_version"), TEXT("uerl.seed.v1"));
			++ExpectedSequence;
			return SendFrame(EMessage::Initialize, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
		}

		SelectedSchema = FUERLBatchSchema();
		if (bDescribePrepared)
		{
			FString CommitIdentityHash;
			if (!WorkerIdentityHash(Projection, CommitIdentityHash, Error)
				|| CommitIdentityHash != PreparedIdentityHash)
			{
				return Fail(TEXT("CONFIG_REJECTED"), TEXT("Initialize commit does not match the described Worker"), Header.Sequence);
			}
			if (!PackRequestedFields(RequestedActions, SelectedSchema.ActionFields, SelectedSchema.ActionWidth, Error)
				|| !PackRequestedFields(RequestedStates, SelectedSchema.StateFields, SelectedSchema.StateWidth, Error))
			{
				FUERLBridgeRequest Abort;
				Abort.Type = EUERLBridgeRequestType::AbortInitialize;
				SubmitWorker(Abort);
				bDescribePrepared = false;
				return Fail(TEXT("SCHEMA_MISMATCH"), Error, Header.Sequence);
			}
		}
		else
		{
			return Fail(TEXT("STATE_VIOLATION"), TEXT("Initialize commit requires a preceding describe"), Header.Sequence);
		}
		SelectedSchema.ResetWidth = Projection.RobotConfig.ResetBindings.Num();
		Projection.ActionWidth = SelectedSchema.ActionWidth;
		Projection.StateWidth = SelectedSchema.StateWidth;
		if (!Compile(Projection.NumSlots, SelectedSchema, Layouts, Error))
		{
			FUERLBridgeRequest Abort;
			Abort.Type = EUERLBridgeRequestType::AbortInitialize;
			SubmitWorker(Abort);
			return Fail(TEXT("SCHEMA_MISMATCH"), Error, Header.Sequence);
		}

		Actions.SetNumZeroed(Projection.NumSlots * Projection.ActionWidth);
		States.SetNumZeroed(Projection.NumSlots * Projection.StateWidth);
		Faults.SetNumZeroed(Projection.NumSlots);
		FUERLBridgeRequest Commit;
		Commit.Type = EUERLBridgeRequestType::CommitInitialize;
		Commit.Sequence = Header.Sequence;
		Commit.Projection = Projection;
		Commit.SelectedSchema = SelectedSchema;
		Commit.States = { States.GetData(), Projection.NumSlots, Projection.StateWidth };
		Commit.Faults = { Faults.GetData(), Projection.NumSlots };
		SubmitWorker(Commit);
		if (!Commit.bOk) { return Fail(WorkerErrorCode(Commit.ErrorCode), Commit.ErrorMessage, Header.Sequence); }
		Episodes = Commit.EpisodeIndices;
		EffectiveProjection = Commit.Projection;
		EffectiveProjection.ActionWidth = SelectedSchema.ActionWidth;
		EffectiveProjection.StateWidth = SelectedSchema.StateWidth;

		TSharedRef<FJsonObject> EffectiveConfig = WorkerConfigJson(EffectiveProjection);
		FString EffectiveHash;
		if (!UERLJson::CanonicalSha256(EffectiveConfig, EffectiveHash, Error)) { return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence); }
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetStringField(TEXT("phase"), TEXT("commit"));
		ResponseJson->SetObjectField(TEXT("selected_protocol"), ProtocolVersionJson());
		ResponseJson->SetObjectField(TEXT("build_identity"), BuildIdentityJson());
		ResponseJson->SetObjectField(TEXT("effective_worker_config"), EffectiveConfig);
		ResponseJson->SetStringField(TEXT("effective_worker_config_hash"), EffectiveHash);
		const TArray<FUERLFieldDescriptor>& AvailableActions = Commit.AvailableActionFields;
		const TArray<FUERLFieldDescriptor>& AvailableStates = Commit.AvailableStateFields;
		const FUERLRobotTopology& AvailableTopology = Commit.AvailableTopology;
		ResponseJson->SetField(TEXT("available_state_schema"), DescriptorArray(AvailableStates));
		ResponseJson->SetField(TEXT("available_action_schema"), DescriptorArray(AvailableActions));
		ResponseJson->SetObjectField(TEXT("topology"), TopologyJson(AvailableTopology));
		TSharedRef<FJsonObject> Selected = MakeShared<FJsonObject>();
		Selected->SetField(TEXT("state"), FieldArray(SelectedSchema.StateFields));
		Selected->SetField(TEXT("action"), FieldArray(SelectedSchema.ActionFields));
		FString StateSchemaJson, ActionSchemaJson, StateSchemaHash, ActionSchemaHash;
		UERLJson::Canonicalize(Selected->Values[TEXT("state")], StateSchemaJson, Error);
		UERLJson::Canonicalize(Selected->Values[TEXT("action")], ActionSchemaJson, Error);
		UERLJson::Sha256Utf8(StateSchemaJson, StateSchemaHash);
		UERLJson::Sha256Utf8(ActionSchemaJson, ActionSchemaHash);
		Selected->SetStringField(TEXT("state_schema_hash"), StateSchemaHash);
		Selected->SetStringField(TEXT("action_schema_hash"), ActionSchemaHash);
		ResponseJson->SetObjectField(TEXT("selected_schemas"), Selected);
		TArray<TSharedPtr<FJsonValue>> LayoutValues;
		for (const FLayout* Layout : { &Layouts.InitialState, &Layouts.StepAction, &Layouts.StepResult,
			&Layouts.ResetRequest, &Layouts.ResetResult })
		{
			LayoutValues.Add(MakeShared<FJsonValueObject>(Layout->ToJson()));
		}
		ResponseJson->SetArrayField(TEXT("layouts"), LayoutValues);
		ResponseJson->SetStringField(TEXT("seed_derivation_version"), TEXT("uerl.seed.v1"));

		TArray<uint8> InitialPayload;
		if (!EncodeState(Layouts.InitialState, { States.GetData(), Projection.NumSlots, Projection.StateWidth },
			{ Faults.GetData(), Projection.NumSlots }, Episodes, nullptr, InitialPayload, Error))
		{
			return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
		}
		State = ESessionState::WaitingReady;
		++ExpectedSequence;
		bDescribePrepared = false;
		return SendFrame(EMessage::Initialize, Response | MoreFrames, Header.Sequence, 0, SerializeJson(ResponseJson))
			&& SendFrame(EMessage::InitialState, Response, Header.Sequence, Layouts.InitialState.LayoutId, InitialPayload);
	}

	bool ProcessReady(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Ready, 0, Code, Error)) { return Fail(Code, Error, Header.Sequence); }
		TSharedPtr<FJsonObject> Json;
		FString ManifestHash;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !CheckKeys(Json, { TEXT("manifest_hash") }, { TEXT("extensions") }, Error)
			|| !Json->TryGetStringField(TEXT("manifest_hash"), ManifestHash) || !IsHex32(ManifestHash))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error.IsEmpty() ? TEXT("invalid manifest_hash") : Error, Header.Sequence);
		}
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Ready;
		RequestDto.Sequence = Header.Sequence;
		RequestDto.ManifestHash = ManifestHash;
		SubmitWorker(RequestDto);
		if (!RequestDto.bOk) { return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence); }
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetBoolField(TEXT("ready"), true);
		State = ESessionState::Ready;
		++ExpectedSequence;
		return SendFrame(EMessage::Ready, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
	}

	bool ProcessStep(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		const double ServerStartSeconds = FPlatformTime::Seconds();
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Step, Layouts.StepAction.LayoutId, Code, Error))
		{
			return Fail(Code, Error, Header.Sequence);
		}
		if (Header.PayloadLength != Layouts.StepAction.PayloadLength)
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Step payload length mismatch"), Header.Sequence);
		}
		const double DecodeStartSeconds = FPlatformTime::Seconds();
		if (!DecodeActions(Layouts.StepAction, Payload,
			{ Actions.GetData(), EffectiveProjection.NumSlots, EffectiveProjection.ActionWidth }, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		int32 StepDecimation = 0;
		if (!DecodeStepDecimation(Layouts.StepAction, Payload, StepDecimation, Error)
			|| StepDecimation < EffectiveProjection.DecimationMin
			|| StepDecimation > EffectiveProjection.DecimationMax)
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error.IsEmpty() ? TEXT("Step decimation is outside the configured range") : Error, Header.Sequence);
		}
		const double ActionDecodeSeconds = FPlatformTime::Seconds() - DecodeStartSeconds;
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Step;
		RequestDto.Sequence = Header.Sequence;
		RequestDto.StepDecimation = StepDecimation;
		RequestDto.Actions = { Actions.GetData(), EffectiveProjection.NumSlots, EffectiveProjection.ActionWidth };
		RequestDto.States = { States.GetData(), EffectiveProjection.NumSlots, EffectiveProjection.StateWidth };
		RequestDto.Faults = { Faults.GetData(), EffectiveProjection.NumSlots };
		RequestDto.StepTiming.ActionDecodeSeconds = ActionDecodeSeconds;
		const double WorkerStartSeconds = FPlatformTime::Seconds();
		SubmitWorker(RequestDto);
		const double WorkerWaitSeconds = FPlatformTime::Seconds() - WorkerStartSeconds;
		if (!RequestDto.bOk) { return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence); }
		TArray<uint8> Result;
		const double EncodeStartSeconds = FPlatformTime::Seconds();
		if (!EncodeState(Layouts.StepResult, RequestDto.States, RequestDto.Faults, Episodes, nullptr, Result, Error))
		{
			return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
		}
		const double ResponseEncodeSeconds = FPlatformTime::Seconds() - EncodeStartSeconds;
		++StepCount;
		++ExpectedSequence;
		const double SendStartSeconds = FPlatformTime::Seconds();
		const bool bSent = SendFrame(EMessage::Step, Response, Header.Sequence, Layouts.StepResult.LayoutId, Result);
		const double ResponseSendSeconds = FPlatformTime::Seconds() - SendStartSeconds;
		const double ServerTotalSeconds = FPlatformTime::Seconds() - ServerStartSeconds;
		if (bSent)
		{
			WriteStepTiming(Header.Sequence, RequestDto.StepTiming, WorkerWaitSeconds,
				ResponseEncodeSeconds, ResponseSendSeconds, ServerTotalSeconds);
		}
		return bSent;
	}

	void WriteStepTiming(
		uint64 Sequence,
		const FUERLStepTiming& Timing,
		double WorkerWaitSeconds,
		double ResponseEncodeSeconds,
		double ResponseSendSeconds,
		double ServerTotalSeconds)
	{
		if (!PerformanceLog) { return; }
		const double AccountedWorkerSeconds = Timing.QueueWaitSeconds + Timing.ActionApplySeconds
			+ Timing.PhysicsFrameSeconds + Timing.ContactSampleSeconds + Timing.StateCollectSeconds;
		TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("request_type"), TEXT("step"));
		Record->SetStringField(TEXT("sequence"), LexToString(Sequence));
		Record->SetNumberField(TEXT("action_decode_s"), Timing.ActionDecodeSeconds);
		Record->SetNumberField(TEXT("worker_wait_s"), WorkerWaitSeconds);
		Record->SetNumberField(TEXT("worker_queue_wait_s"), Timing.QueueWaitSeconds);
		Record->SetNumberField(TEXT("action_apply_s"), Timing.ActionApplySeconds);
		Record->SetNumberField(TEXT("action_apply_count"), Timing.ActionApplyCount);
		Record->SetNumberField(TEXT("physics_frame_s"), Timing.PhysicsFrameSeconds);
		Record->SetNumberField(TEXT("contact_sample_s"), Timing.ContactSampleSeconds);
		Record->SetNumberField(TEXT("state_collect_s"), Timing.StateCollectSeconds);
		Record->SetNumberField(TEXT("worker_other_s"),
			FMath::Max(0.0, WorkerWaitSeconds - AccountedWorkerSeconds));
		Record->SetNumberField(TEXT("response_encode_s"), ResponseEncodeSeconds);
		Record->SetNumberField(TEXT("response_send_s"), ResponseSendSeconds);
		Record->SetNumberField(TEXT("server_total_s"), ServerTotalSeconds);
		TArray<uint8> Line = SerializeJson(Record);
		Line.Add(static_cast<uint8>('\n'));
		PerformanceLog->Serialize(Line.GetData(), Line.Num());
	}

	void WriteResetTiming(
		uint64 Sequence,
		int32 ResetRows,
		const FUERLResetTiming& Timing,
		double WorkerWaitSeconds,
		double ResponseEncodeSeconds,
		double ResponseSendSeconds,
		double ServerTotalSeconds)
	{
		if (!PerformanceLog) { return; }
		const double AccountedWorkerSeconds = Timing.QueueWaitSeconds + Timing.ResetSlotsSeconds
			+ Timing.StateCollectSeconds + Timing.ValidateSlotsSeconds + Timing.SafetySeconds
			+ Timing.EpisodeCopySeconds;
		TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("request_type"), TEXT("reset"));
		Record->SetStringField(TEXT("sequence"), LexToString(Sequence));
		Record->SetNumberField(TEXT("reset_rows"), ResetRows);
		Record->SetNumberField(TEXT("request_decode_s"), Timing.RequestDecodeSeconds);
		Record->SetNumberField(TEXT("worker_wait_s"), WorkerWaitSeconds);
		Record->SetNumberField(TEXT("worker_queue_wait_s"), Timing.QueueWaitSeconds);
		Record->SetNumberField(TEXT("reset_slots_s"), Timing.ResetSlotsSeconds);
		Record->SetNumberField(TEXT("state_collect_s"), Timing.StateCollectSeconds);
		Record->SetNumberField(TEXT("validate_slots_s"), Timing.ValidateSlotsSeconds);
		Record->SetNumberField(TEXT("safety_s"), Timing.SafetySeconds);
		Record->SetNumberField(TEXT("episode_copy_s"), Timing.EpisodeCopySeconds);
		Record->SetNumberField(TEXT("worker_other_s"),
			WorkerWaitSeconds - AccountedWorkerSeconds);
		Record->SetNumberField(TEXT("response_encode_s"), ResponseEncodeSeconds);
		Record->SetNumberField(TEXT("response_send_s"), ResponseSendSeconds);
		Record->SetNumberField(TEXT("server_total_s"), ServerTotalSeconds);
		TArray<uint8> Line = SerializeJson(Record);
		Line.Add(static_cast<uint8>('\n'));
		PerformanceLog->Serialize(Line.GetData(), Line.Num());
	}

	bool ProcessReset(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		const double ServerStartSeconds = FPlatformTime::Seconds();
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Reset, Layouts.ResetRequest.LayoutId, Code, Error))
		{
			return Fail(Code, Error, Header.Sequence);
		}
		if (Header.PayloadLength != Layouts.ResetRequest.PayloadLength)
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Reset payload length mismatch"), Header.Sequence);
		}
		const double DecodeStartSeconds = FPlatformTime::Seconds();
		TArray<int32> ResetSlots;
		if (!DecodeResetMask(Layouts.ResetRequest, Payload, EffectiveProjection.NumSlots, ResetSlots, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		TArray<uint16> TerrainLevels;
		if (!DecodeTerrainLevels(Layouts.ResetRequest, Payload, EffectiveProjection.NumSlots, TerrainLevels, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		TArray<float> ResetValues;
		if (!DecodeResetValues(Layouts.ResetRequest, Payload, EffectiveProjection.NumSlots, ResetValues, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		const double RequestDecodeSeconds = FPlatformTime::Seconds() - DecodeStartSeconds;
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Reset;
		RequestDto.Sequence = Header.Sequence;
		RequestDto.ResetSlots = ResetSlots;
		RequestDto.TerrainLevels = TerrainLevels;
		RequestDto.ResetValues = MoveTemp(ResetValues);
		RequestDto.States = { States.GetData(), EffectiveProjection.NumSlots, EffectiveProjection.StateWidth };
		RequestDto.Faults = { Faults.GetData(), EffectiveProjection.NumSlots };
		RequestDto.ResetTiming.RequestDecodeSeconds = RequestDecodeSeconds;
		const double WorkerStartSeconds = FPlatformTime::Seconds();
		SubmitWorker(RequestDto);
		const double WorkerWaitSeconds = FPlatformTime::Seconds() - WorkerStartSeconds;
		if (!RequestDto.bOk) { return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence); }
		Episodes = RequestDto.EpisodeIndices;
		TArray<uint8> Result;
		const double EncodeStartSeconds = FPlatformTime::Seconds();
		if (!EncodeState(Layouts.ResetResult, RequestDto.States, RequestDto.Faults, Episodes, &ResetSlots, Result, Error))
		{
			return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
		}
		const double ResponseEncodeSeconds = FPlatformTime::Seconds() - EncodeStartSeconds;
		++ExpectedSequence;
		const double SendStartSeconds = FPlatformTime::Seconds();
		const bool bSent = SendFrame(EMessage::Reset, Response, Header.Sequence, Layouts.ResetResult.LayoutId, Result);
		const double ResponseSendSeconds = FPlatformTime::Seconds() - SendStartSeconds;
		const double ServerTotalSeconds = FPlatformTime::Seconds() - ServerStartSeconds;
		if (bSent)
		{
			WriteResetTiming(Header.Sequence, ResetSlots.Num(), RequestDto.ResetTiming, WorkerWaitSeconds,
				ResponseEncodeSeconds, ResponseSendSeconds, ServerTotalSeconds);
		}
		return bSent;
	}

	bool ProcessEvent(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Event, 0, Code, Error))
		{
			return Fail(Code, Error, Header.Sequence);
		}
		TSharedPtr<FJsonObject> Json;
		FUERLEventBatch Event;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !ParseEvent(Json, EffectiveProjection.NumSlots, Event, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Event;
		RequestDto.Sequence = Header.Sequence;
		RequestDto.Event = MoveTemp(Event);
		SubmitWorker(RequestDto);
		if (!RequestDto.bOk)
		{
			return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence);
		}
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetBoolField(TEXT("applied"), true);
		ResponseJson->SetStringField(TEXT("kind"), UERLEventKindName(RequestDto.Event.Kind));
		if (RequestDto.Event.Kind == EUERLEventKind::TerrainTierParams)
		{
			TArray<TSharedPtr<FJsonValue>> TerrainLevels;
			for (int32 Row = 0; Row < RequestDto.Event.SlotIds.Num(); ++Row)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("slot_id"), RequestDto.Event.SlotIds[Row]);
				Item->SetNumberField(TEXT("level"), RequestDto.Event.AppliedTerrainLevels[Row]);
				TerrainLevels.Add(MakeShared<FJsonValueObject>(Item));
			}
			ResponseJson->SetArrayField(TEXT("terrain_levels"), TerrainLevels);
		}
		++ExpectedSequence;
		return SendFrame(EMessage::Event, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
	}

	bool ProcessShutdown(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Shutdown, 0, Code, Error)) { return Fail(Code, Error, Header.Sequence); }
		TSharedPtr<FJsonObject> Json;
		FString Reason;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !CheckKeys(Json, { TEXT("reason") }, { TEXT("extensions") }, Error)
			|| !Json->TryGetStringField(TEXT("reason"), Reason) || Reason.IsEmpty())
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error.IsEmpty() ? TEXT("Shutdown reason is required") : Error, Header.Sequence);
		}
		State = ESessionState::Closing;
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Shutdown;
		RequestDto.Sequence = Header.Sequence;
		SubmitWorker(RequestDto);
		if (!RequestDto.bOk) { return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence); }
		Episodes = RequestDto.EpisodeIndices;
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetStringField(TEXT("final_step_count"), LexToString(StepCount));
		TArray<TSharedPtr<FJsonValue>> EpisodeValues;
		for (uint64 Episode : Episodes) { EpisodeValues.Add(MakeShared<FJsonValueString>(LexToString(Episode))); }
		ResponseJson->SetArrayField(TEXT("final_episode_indices"), EpisodeValues);
		if (PerformanceLog) { PerformanceLog->Flush(); }
		const bool bSent = SendFrame(EMessage::Shutdown, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
		bGracefulShutdown = bSent;
		return false;
	}

	FUERLBridgeServerConfig Config;
	FUERLBridgeCapabilities Capabilities;
	IBridgeRequestHandler* Handler = nullptr;
	FRunnableThread* Thread = nullptr;
	TUniquePtr<IUERLTransport> Transport;
	TUniquePtr<FArchive> PerformanceLog;
	FThreadSafeBool bStopRequested{ false };
	bool bGracefulShutdown = false;
	bool bTerminalErrorResponseSent = false;
	ESessionState State = ESessionState::Connected;
	FGuid Session;
	uint64 ExpectedSequence = 1;
	uint64 StepCount = 0;
	bool bDescribePrepared = false;
	FString PreparedIdentityHash;
	FUERLWorkerProjection PreparedProjection;
	TArray<FUERLFieldDescriptor> PreparedActionFields;
	TArray<FUERLFieldDescriptor> PreparedStateFields;
	FUERLRobotTopology PreparedTopology;
	FUERLWorkerProjection EffectiveProjection;
	FUERLBatchSchema SelectedSchema;
	FSet Layouts;
	TArray<float> Actions;
	TArray<float> States;
	TArray<uint8> Faults;
	TArray<uint64> Episodes;
	double ActiveDeadlineSeconds = 0.0;
};

FUERLBridgeServer::FUERLBridgeServer() : Impl(MakeUnique<FImpl>()) {}
FUERLBridgeServer::~FUERLBridgeServer() = default;

bool FUERLBridgeServer::Start(
	const FUERLBridgeServerConfig& Config,
	const FUERLBridgeCapabilities& Capabilities,
	IBridgeRequestHandler& Handler,
	FString& OutError)
{
	return Impl->Start(Config, Capabilities, Handler, OutError);
}

void FUERLBridgeServer::Stop() { Impl->Shutdown(); }
bool FUERLBridgeServer::IsRunning() const { return Impl->IsRunning(); }
