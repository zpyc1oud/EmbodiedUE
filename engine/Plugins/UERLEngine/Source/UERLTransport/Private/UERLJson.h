#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

namespace UERLJson
{
	bool ParseStrictObject(const TArray<uint8>& Utf8, TSharedPtr<FJsonObject>& OutObject, FString& OutError);
	bool Canonicalize(const TSharedPtr<FJsonValue>& Value, FString& OutJson, FString& OutError);
	bool CanonicalizeObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson, FString& OutError);
	bool Sha256Utf8(const FString& Text, FString& OutHex);
	bool CanonicalSha256(const TSharedPtr<FJsonObject>& Object, FString& OutHex, FString& OutError);
	TArray<uint8> ToUtf8(const FString& Text);
}
