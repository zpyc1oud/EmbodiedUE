#pragma once

#include "CoreMinimal.h"

/** Define the versioned frame envelope shared by the Python client and UE Worker. */
namespace UERLProtocol
{
	/** Define protocol constants and payload limits for the SocketBridge frame boundary. */
	constexpr uint32 Magic = 0x5545524C;
	constexpr uint16 Major = 2;
	constexpr uint16 Minor = 0;
	constexpr int32 FrameHeaderSize = 48;
	constexpr uint32 MaxControlPayloadBytes = 1024 * 1024;
	constexpr uint32 MaxDataPayloadBytes = 64 * 1024 * 1024;

	/** Identify the payload semantics carried by a frame. */
	enum class EMessage : uint16
	{
		Hello = 0x0001,
		Initialize = 0x0002,
		InitialState = 0x0003,
		Ready = 0x0004,
		Shutdown = 0x0005,
		Error = 0x00FF,
		Step = 0x0101,
		Reset = 0x0102,
		Event = 0x0103,
	};

	/** Identify the direction and transaction flags carried by a frame. */
	enum EFlags : uint16
	{
		Request = 0x0001,
		Response = 0x0002,
		Error = 0x0004,
		MoreFrames = 0x0008,
	};

	/**
	 * Represent the fixed 48-byte frame header.
	 *
	 * Numeric fields are encoded in network byte order. Hello uses protocol
	 * version 0.0; later frames use the negotiated version.
	 */
	struct FFrameHeader
	{
		/** Store the protocol major version encoded in this frame. */
		uint16 ProtocolMajor = Major;
		/** Store the protocol minor version encoded in this frame. */
		uint16 ProtocolMinor = Minor;
		/** Identify the control or data message carried by this frame. */
		EMessage Message = EMessage::Error;
		/** Store exactly one direction flag and any valid transaction flags. */
		uint16 Flags = 0;
		/** Store the payload length in bytes. */
		uint32 PayloadLength = 0;
		/** Identify the request and matching response transaction. */
		uint64 Sequence = 0;
		/** Identify the client session across every post-Hello frame. */
		FGuid Session;
		/** Identify the negotiated binary layout, or zero for control frames. */
		uint64 LayoutId = 0;

		/** Return whether the message uses the control-payload limit. */
		bool IsControl() const;
		/**
		 * Validate message, flags, and payload limits without decoding a payload.
		 *
		 * @param OutError Receive a stable diagnostic when validation fails.
		 * @return true when the header is valid for the protocol boundary.
		 */
		bool Validate(FString& OutError) const;
		/** Encode this header into the fixed-size network-order representation. */
		void Encode(uint8 OutBytes[FrameHeaderSize]) const;
		/**
		 * Decode and validate one fixed-size network-order header.
		 *
		 * @param Bytes Receive exactly FrameHeaderSize bytes.
		 * @param OutHeader Receive the decoded header.
		 * @param OutError Receive a stable diagnostic when decoding fails.
		 * @return true when the header is valid for the protocol boundary.
		 */
		static bool Decode(const uint8 Bytes[FrameHeaderSize], FFrameHeader& OutHeader, FString& OutError);
	};
}
