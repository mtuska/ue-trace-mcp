// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#pragma once

#include "TraceDigestPCH.h"

namespace TraceDigest
{

// Thin JSON writer over an FStringBuilderBase. We use this instead of UE's
// FJsonWriter because we want compact, deterministic output that's cheap to
// stream to stdout without allocating intermediate FJsonValue trees for
// every per-event record (which can be in the millions).
class FJsonOut
{
public:
	void BeginObject();
	void EndObject();
	void BeginArray();
	void EndArray();

	void Key(const TCHAR* K);
	void Str(const FString& V);
	void Num(double V);
	void Int(int64 V);
	void Bool(bool V);
	void Null();

	void KeyStr (const TCHAR* K, const FString& V);
	void KeyNum (const TCHAR* K, double V);
	void KeyInt (const TCHAR* K, int64 V);
	void KeyBool(const TCHAR* K, bool V);

	// Returns the assembled JSON. May be called multiple times if you want to
	// stream chunks; in that case call Reset() between flushes.
	FString ToString() const;
	void Reset();

private:
	void Sep();

	FString Buffer;
	// Stack of "needs comma before next element" flags, one per nesting level.
	TArray<bool> NeedsComma;
	bool bAfterKey = false; // suppress comma between key and value
};

} // namespace TraceDigest
