#include "UERLJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <algorithm>
#include <charconv>
#include <string>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
#include <openssl/sha.h>
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	// Match the RFC 8785/ECMAScript number spelling used by the Python
	// canonicalizer; equivalent numeric values must hash to identical bytes.
	std::string NormalizeEcmaNumber(const ANSICHAR* Begin, const ANSICHAR* End)
	{
		std::string Raw(Begin, End);
		const size_t ExponentAt = Raw.find('e');
		if (ExponentAt == std::string::npos) { return Raw; }
		const std::string Mantissa = Raw.substr(0, ExponentAt);
		const int32 Exponent = FCStringAnsi::Atoi(Raw.c_str() + ExponentAt + 1);
		const bool bNegative = !Mantissa.empty() && Mantissa[0] == '-';
		std::string Digits = Mantissa.substr(bNegative ? 1 : 0);
		Digits.erase(std::remove(Digits.begin(), Digits.end(), '.'), Digits.end());
		if (Exponent >= -6 && Exponent < 21)
		{
			const int32 DecimalAt = 1 + Exponent;
			std::string Fixed = bNegative ? "-" : "";
			if (DecimalAt <= 0)
			{
				Fixed += "0.";
				Fixed.append(-DecimalAt, '0');
				Fixed += Digits;
			}
			else if (DecimalAt >= static_cast<int32>(Digits.size()))
			{
				Fixed += Digits;
				Fixed.append(DecimalAt - Digits.size(), '0');
			}
			else
			{
				Fixed += Digits.substr(0, DecimalAt) + "." + Digits.substr(DecimalAt);
			}
			return Fixed;
		}
		return Mantissa + "e" + (Exponent >= 0 ? "+" : "") + std::to_string(Exponent);
	}

	bool IsValidUtf8(const TArray<uint8>& Bytes)
	{
		// Validate code-point ranges explicitly because FString conversion alone
		// must not silently replace malformed wire text.
		for (int32 Index = 0; Index < Bytes.Num();)
		{
			const uint8 Lead = Bytes[Index++];
			if (Lead <= 0x7f) { continue; }
			int32 Continuations = 0;
			uint32 Codepoint = 0;
			uint32 Minimum = 0;
			if ((Lead & 0xe0) == 0xc0) { Continuations = 1; Codepoint = Lead & 0x1f; Minimum = 0x80; }
			else if ((Lead & 0xf0) == 0xe0) { Continuations = 2; Codepoint = Lead & 0x0f; Minimum = 0x800; }
			else if ((Lead & 0xf8) == 0xf0) { Continuations = 3; Codepoint = Lead & 0x07; Minimum = 0x10000; }
			else { return false; }
			if (Index + Continuations > Bytes.Num()) { return false; }
			for (int32 Part = 0; Part < Continuations; ++Part)
			{
				const uint8 Byte = Bytes[Index++];
				if ((Byte & 0xc0) != 0x80) { return false; }
				Codepoint = (Codepoint << 6) | (Byte & 0x3f);
			}
			if (Codepoint < Minimum || Codepoint > 0x10ffff || (Codepoint >= 0xd800 && Codepoint <= 0xdfff))
			{
				return false;
			}
		}
		return true;
	}

	class FDuplicateKeyScanner
	{
	public:
		explicit FDuplicateKeyScanner(const FString& InText) : Text(InText) {}

		bool Scan(FString& OutError)
		{
			// Run a small structural pass before UE's parser so duplicate keys
			// and trailing content cannot be accepted by implementation quirks.
			SkipSpace();
			if (!Value(OutError)) { return false; }
			SkipSpace();
			if (Position != Text.Len())
			{
				OutError = TEXT("trailing JSON content");
				return false;
			}
			return true;
		}

	private:
		void SkipSpace()
		{
			while (Position < Text.Len() && FChar::IsWhitespace(Text[Position])) { ++Position; }
		}

		bool Consume(TCHAR Expected)
		{
			SkipSpace();
			if (Position < Text.Len() && Text[Position] == Expected) { ++Position; return true; }
			return false;
		}

		bool String(FString& OutValue, FString& OutError)
		{
			SkipSpace();
			if (Position >= Text.Len() || Text[Position++] != TEXT('"')) { OutError = TEXT("expected JSON string"); return false; }
			while (Position < Text.Len())
			{
				const TCHAR Character = Text[Position++];
				if (Character == TEXT('"')) { return true; }
				if (Character < 0x20) { OutError = TEXT("unescaped control character in JSON string"); return false; }
				if (Character != TEXT('\\')) { OutValue.AppendChar(Character); continue; }
				if (Position >= Text.Len()) { OutError = TEXT("truncated JSON escape"); return false; }
				const TCHAR Escape = Text[Position++];
				switch (Escape)
				{
				case TEXT('"'): OutValue.AppendChar(TEXT('"')); break;
				case TEXT('\\'): OutValue.AppendChar(TEXT('\\')); break;
				case TEXT('/'): OutValue.AppendChar(TEXT('/')); break;
				case TEXT('b'): OutValue.AppendChar(TEXT('\b')); break;
				case TEXT('f'): OutValue.AppendChar(TEXT('\f')); break;
				case TEXT('n'): OutValue.AppendChar(TEXT('\n')); break;
				case TEXT('r'): OutValue.AppendChar(TEXT('\r')); break;
				case TEXT('t'): OutValue.AppendChar(TEXT('\t')); break;
				case TEXT('u'):
				{
					if (Position + 4 > Text.Len()) { OutError = TEXT("truncated unicode escape"); return false; }
					uint32 Code = 0;
					for (int32 Digit = 0; Digit < 4; ++Digit)
					{
						const int32 Hex = FParse::HexDigit(Text[Position++]);
						if (Hex < 0) { OutError = TEXT("invalid unicode escape"); return false; }
						Code = (Code << 4) | static_cast<uint32>(Hex);
					}
					OutValue.AppendChar(static_cast<TCHAR>(Code));
					break;
				}
				default: OutError = TEXT("invalid JSON escape"); return false;
				}
			}
			OutError = TEXT("unterminated JSON string");
			return false;
		}

		bool Object(FString& OutError)
		{
			++Position;
			TSet<FString> Keys;
			SkipSpace();
			if (Consume(TEXT('}'))) { return true; }
			for (;;)
			{
				FString Key;
				if (!String(Key, OutError)) { return false; }
				if (Keys.Contains(Key)) { OutError = FString::Printf(TEXT("duplicate JSON key '%s'"), *Key); return false; }
				Keys.Add(Key);
				if (!Consume(TEXT(':'))) { OutError = TEXT("expected ':' after JSON key"); return false; }
				if (!Value(OutError)) { return false; }
				if (Consume(TEXT('}'))) { return true; }
				if (!Consume(TEXT(','))) { OutError = TEXT("expected ',' in JSON object"); return false; }
			}
		}

		bool Array(FString& OutError)
		{
			++Position;
			SkipSpace();
			if (Consume(TEXT(']'))) { return true; }
			for (;;)
			{
				if (!Value(OutError)) { return false; }
				if (Consume(TEXT(']'))) { return true; }
				if (!Consume(TEXT(','))) { OutError = TEXT("expected ',' in JSON array"); return false; }
			}
		}

		bool Primitive(FString& OutError)
		{
			const int32 Start = Position;
			while (Position < Text.Len() && !FChar::IsWhitespace(Text[Position])
				&& Text[Position] != TEXT(',') && Text[Position] != TEXT(']') && Text[Position] != TEXT('}'))
			{
				++Position;
			}
			if (Start == Position) { OutError = TEXT("expected JSON value"); return false; }
			const FString Token = Text.Mid(Start, Position - Start);
			if (!Token.Contains(TEXT(".")) && !Token.Contains(TEXT("e")) && !Token.Contains(TEXT("E"))
				&& Token != TEXT("true") && Token != TEXT("false") && Token != TEXT("null"))
			{
				const FString Digits = Token.StartsWith(TEXT("-")) ? Token.Mid(1) : Token;
				const FString MaxSafeInteger = TEXT("9007199254740991");
				if (Digits.Len() > MaxSafeInteger.Len()
					|| (Digits.Len() == MaxSafeInteger.Len() && Digits.Compare(MaxSafeInteger) > 0))
				{
					OutError = TEXT("JSON integer exceeds the interoperable safe range");
					return false;
				}
			}
			return true;
		}

		bool Value(FString& OutError)
		{
			SkipSpace();
			if (Position >= Text.Len()) { OutError = TEXT("missing JSON value"); return false; }
			if (Text[Position] == TEXT('{')) { return Object(OutError); }
			if (Text[Position] == TEXT('[')) { return Array(OutError); }
			if (Text[Position] == TEXT('"')) { FString Ignored; return String(Ignored, OutError); }
			return Primitive(OutError);
		}

		const FString& Text;
		int32 Position = 0;
	};

	void AppendEscaped(const FString& Text, FString& Out)
	{
		Out.AppendChar(TEXT('"'));
		for (TCHAR Character : Text)
		{
			switch (Character)
			{
			case TEXT('"'): Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\b'): Out += TEXT("\\b"); break;
			case TEXT('\f'): Out += TEXT("\\f"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (Character < 0x20) { Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character)); }
				else { Out.AppendChar(Character); }
				break;
			}
		}
		Out.AppendChar(TEXT('"'));
	}

	bool CanonicalValue(const TSharedPtr<FJsonValue>& Value, FString& Out, FString& OutError)
	{
		// Recursively emit one deterministic representation for hashing; do not
		// delegate to the engine serializer because its formatting is not frozen.
		if (!Value.IsValid()) { OutError = TEXT("null JSON value pointer"); return false; }
		switch (Value->Type)
		{
		case EJson::Null: Out += TEXT("null"); return true;
		case EJson::Boolean: Out += Value->AsBool() ? TEXT("true") : TEXT("false"); return true;
		case EJson::String: AppendEscaped(Value->AsString(), Out); return true;
		case EJson::Number:
		{
			const double Number = Value->AsNumber();
			if (!FMath::IsFinite(Number)) { OutError = TEXT("non-finite JSON number"); return false; }
			if (Number == 0.0)
			{
				Out += TEXT("0");
				return true;
			}
			ANSICHAR Buffer[64];
			const std::to_chars_result Result = std::to_chars(
				Buffer, Buffer + UE_ARRAY_COUNT(Buffer), Number, std::chars_format::general);
			if (Result.ec != std::errc()) { OutError = TEXT("cannot canonicalize JSON number"); return false; }
			const std::string CanonicalNumber = NormalizeEcmaNumber(Buffer, Result.ptr);
			Out += ANSI_TO_TCHAR(CanonicalNumber.c_str());
			return true;
		}
		case EJson::Array:
		{
			Out.AppendChar(TEXT('['));
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (Index > 0) { Out.AppendChar(TEXT(',')); }
				if (!CanonicalValue(Values[Index], Out, OutError)) { return false; }
			}
			Out.AppendChar(TEXT(']'));
			return true;
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			TArray<FString> Keys;
			for (const auto& Pair : Object->Values) { Keys.Add(FString(Pair.Key)); }
			Keys.Sort();
			Out.AppendChar(TEXT('{'));
			for (int32 Index = 0; Index < Keys.Num(); ++Index)
			{
				if (Index > 0) { Out.AppendChar(TEXT(',')); }
				AppendEscaped(Keys[Index], Out);
				Out.AppendChar(TEXT(':'));
				if (!CanonicalValue(Object->TryGetField(Keys[Index]), Out, OutError)) { return false; }
			}
			Out.AppendChar(TEXT('}'));
			return true;
		}
		default: OutError = TEXT("unsupported JSON value type"); return false;
		}
	}
}

bool UERLJson::ParseStrictObject(const TArray<uint8>& Utf8, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
{
	// Keep strict byte validation at the external JSON boundary before handing
	// the decoded object to protocol and Worker code.
	if (Utf8.IsEmpty() || Utf8.Contains(0) || !IsValidUtf8(Utf8))
	{
		OutError = TEXT("empty, invalid UTF-8, or embedded NUL JSON");
		return false;
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Utf8.GetData()), Utf8.Num());
	const FString Text(Converted.Length(), Converted.Get());
	if (!FDuplicateKeyScanner(Text).Scan(OutError)) { return false; }
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
	{
		OutError = TEXT("invalid UTF-8 JSON object");
		return false;
	}
	return true;
}

bool UERLJson::Canonicalize(const TSharedPtr<FJsonValue>& Value, FString& OutJson, FString& OutError)
{
	OutJson.Reset();
	return CanonicalValue(Value, OutJson, OutError);
}

bool UERLJson::CanonicalizeObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson, FString& OutError)
{
	return Canonicalize(MakeShared<FJsonValueObject>(Object), OutJson, OutError);
}

bool UERLJson::Sha256Utf8(const FString& Text, FString& OutHex)
{
	// Hash UTF-8 canonical bytes and expose lowercase hex for stable DTO fields.
	FTCHARToUTF8 Bytes(*Text);
	uint8 Digest[SHA256_DIGEST_LENGTH];
	if (!SHA256(reinterpret_cast<const unsigned char*>(Bytes.Get()), Bytes.Length(), Digest)) { return false; }
	OutHex = BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
	return true;
}

bool UERLJson::CanonicalSha256(const TSharedPtr<FJsonObject>& Object, FString& OutHex, FString& OutError)
{
	FString Canonical;
	return CanonicalizeObject(Object, Canonical, OutError) && Sha256Utf8(Canonical, OutHex);
}

TArray<uint8> UERLJson::ToUtf8(const FString& Text)
{
	FTCHARToUTF8 Converted(*Text);
	TArray<uint8> Result;
	Result.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	return Result;
}
