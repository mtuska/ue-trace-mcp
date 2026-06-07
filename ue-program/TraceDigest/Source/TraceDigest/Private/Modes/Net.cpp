// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "Modes.h"
#include "Args.h"
#include "JsonOut.h"

#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/NetProfiler.h"

using namespace TraceServices;

namespace TraceDigest
{

// INetProfilerProvider is the biggest API surface of the v0.5 additions —
// hierarchical (instance → connection → packet) with bit-packed event
// content. We expose four views as separate MCP tools but back them with
// one C++ mode file:
//
//   * instances   — list game instances (server/client, replication backend)
//   * connections — list connections for one game instance
//   * packets     — windowed enumeration of packets on one connection
//                   in one direction (outgoing/incoming)
//   * objects     — list replicated object instances per game instance
//
// All views are session-scope only (no per-provider lock).

static const TCHAR* ConnectionModeName(ENetProfilerConnectionMode M)
{
	switch (M)
	{
		case ENetProfilerConnectionMode::Outgoing: return TEXT("outgoing");
		case ENetProfilerConnectionMode::Incoming: return TEXT("incoming");
		default:                                   return TEXT("unknown");
	}
}

static ENetProfilerConnectionMode ParseDirection(const FString& In)
{
	if (In.Equals(TEXT("incoming"), ESearchCase::IgnoreCase))
		return ENetProfilerConnectionMode::Incoming;
	return ENetProfilerConnectionMode::Outgoing;
}

static const TCHAR* DeliveryName(ENetProfilerDeliveryStatus S)
{
	switch (S)
	{
		case ENetProfilerDeliveryStatus::Unknown:   return TEXT("unknown");
		case ENetProfilerDeliveryStatus::Delivered: return TEXT("delivered");
		case ENetProfilerDeliveryStatus::Dropped:   return TEXT("dropped");
		default:                                    return TEXT("invalid");
	}
}

static const TCHAR* ConnectionStateName(ENetProfilerConnectionState S)
{
	switch (S)
	{
		case ENetProfilerConnectionState::USOCK_Invalid: return TEXT("invalid");
		case ENetProfilerConnectionState::USOCK_Closed:  return TEXT("closed");
		case ENetProfilerConnectionState::USOCK_Pending: return TEXT("pending");
		case ENetProfilerConnectionState::USOCK_Open:    return TEXT("open");
		default:                                         return TEXT("unknown");
	}
}

static void EmitInstances(const INetProfilerProvider& Provider, FJsonOut& Json)
{
	Json.KeyInt(TEXT("instance_count"), static_cast<int64>(Provider.GetGameInstanceCount()));
	Json.Key(TEXT("events"));
	Json.BeginArray();
	Provider.ReadGameInstances([&](const FNetProfilerGameInstance& G)
	{
		Json.BeginObject();
		Json.KeyInt (TEXT("game_instance_index"), static_cast<int64>(G.GameInstanceIndex));
		Json.KeyInt (TEXT("game_instance_id"),    static_cast<int64>(G.GameInstanceId));
		Json.KeyStr (TEXT("name"),                G.InstanceName ? FString(G.InstanceName) : FString());
		Json.KeyBool(TEXT("is_server"),           G.bIsServer);
		Json.KeyBool(TEXT("is_iris"),             G.bIsUsingIrisReplication);
		Json.KeyNum (TEXT("life_begin_ms"),       G.LifeTime.Begin * 1000.0);
		Json.KeyNum (TEXT("life_end_ms"),         FMath::IsFinite(G.LifeTime.End) ? G.LifeTime.End * 1000.0 : -1.0);
		Json.EndObject();
	});
	Json.EndArray();
}

static void EmitConnections(const INetProfilerProvider& Provider, const FArgs& Args, FJsonOut& Json)
{
	const int32 InstanceIdx = Args.GameInstanceId;
	Json.KeyInt(TEXT("game_instance_filter"), static_cast<int64>(InstanceIdx));

	if (InstanceIdx < 0)
	{
		// Default behaviour: aggregate across every instance. Call site
		// can also pass a specific game_instance_id to scope.
		const uint32 N = Provider.GetGameInstanceCount();
		Json.Key(TEXT("events"));
		Json.BeginArray();
		for (uint32 i = 0; i < N; ++i)
		{
			Provider.ReadConnections(i, [&](const FNetProfilerConnection& C)
			{
				Json.BeginObject();
				Json.KeyInt (TEXT("game_instance_index"), static_cast<int64>(C.GameInstanceIndex));
				Json.KeyInt (TEXT("connection_index"),    static_cast<int64>(C.ConnectionIndex));
				Json.KeyInt (TEXT("connection_id"),       static_cast<int64>(C.ConnectionId));
				Json.KeyStr (TEXT("name"),                C.Name ? FString(C.Name) : FString());
				Json.KeyStr (TEXT("address"),             C.AddressString ? FString(C.AddressString) : FString());
				Json.KeyBool(TEXT("has_incoming"),        static_cast<bool>(C.bHasIncomingData));
				Json.KeyBool(TEXT("has_outgoing"),        static_cast<bool>(C.bHasOutgoingData));
				Json.KeyNum (TEXT("life_begin_ms"),       C.LifeTime.Begin * 1000.0);
				Json.KeyNum (TEXT("life_end_ms"),         FMath::IsFinite(C.LifeTime.End) ? C.LifeTime.End * 1000.0 : -1.0);
				Json.EndObject();
			});
		}
		Json.EndArray();
		return;
	}

	Json.KeyInt(TEXT("connection_count"),
		static_cast<int64>(Provider.GetConnectionCount(static_cast<uint32>(InstanceIdx))));

	Json.Key(TEXT("events"));
	Json.BeginArray();
	Provider.ReadConnections(static_cast<uint32>(InstanceIdx),
		[&](const FNetProfilerConnection& C)
		{
			Json.BeginObject();
			Json.KeyInt (TEXT("game_instance_index"), static_cast<int64>(C.GameInstanceIndex));
			Json.KeyInt (TEXT("connection_index"),    static_cast<int64>(C.ConnectionIndex));
			Json.KeyInt (TEXT("connection_id"),       static_cast<int64>(C.ConnectionId));
			Json.KeyStr (TEXT("name"),                C.Name ? FString(C.Name) : FString());
			Json.KeyStr (TEXT("address"),             C.AddressString ? FString(C.AddressString) : FString());
			Json.KeyBool(TEXT("has_incoming"),        static_cast<bool>(C.bHasIncomingData));
			Json.KeyBool(TEXT("has_outgoing"),        static_cast<bool>(C.bHasOutgoingData));
			Json.KeyNum (TEXT("life_begin_ms"),       C.LifeTime.Begin * 1000.0);
			Json.KeyNum (TEXT("life_end_ms"),         FMath::IsFinite(C.LifeTime.End) ? C.LifeTime.End * 1000.0 : -1.0);
			Json.EndObject();
		});
	Json.EndArray();
}

static void EmitPackets(const INetProfilerProvider& Provider, const FArgs& Args, FJsonOut& Json)
{
	const uint32 ConnIdx = static_cast<uint32>(Args.ConnectionId);
	const ENetProfilerConnectionMode Dir = ParseDirection(Args.Direction);
	const uint32 Total = Provider.GetPacketCount(ConnIdx, Dir);

	Json.KeyInt (TEXT("connection_index"), static_cast<int64>(ConnIdx));
	Json.KeyStr (TEXT("direction"),        FString(ConnectionModeName(Dir)));
	Json.KeyInt (TEXT("total_packets"),    static_cast<int64>(Total));

	// Default window: last min(Total, 500) packets so the response is
	// bounded on 10⁶+ packet captures.
	const int32 Limit  = Args.Limit > 0 ? Args.Limit : 500;
	uint32 Start = Args.PacketStart >= 0
		? static_cast<uint32>(Args.PacketStart)
		: (Total > static_cast<uint32>(Limit) ? Total - static_cast<uint32>(Limit) : 0u);
	uint32 End = Args.PacketEnd >= 0
		? static_cast<uint32>(Args.PacketEnd)
		: (Total > 0 ? Total - 1u : 0u);
	if (Start > End) Start = End;

	Json.KeyInt(TEXT("window_start"), static_cast<int64>(Start));
	Json.KeyInt(TEXT("window_end"),   static_cast<int64>(End));

	int32 Emitted = 0;
	Json.Key(TEXT("events"));
	Json.BeginArray();
	if (Total > 0)
	{
		Provider.EnumeratePackets(ConnIdx, Dir, Start, End,
			[&](const FNetProfilerPacket& P)
			{
				if (Emitted >= Limit) return;
				Json.BeginObject();
				Json.KeyNum (TEXT("time_ms"),                 P.TimeStamp * 1000.0);
				Json.KeyInt (TEXT("sequence_number"),         static_cast<int64>(P.SequenceNumber));
				Json.KeyInt (TEXT("content_size_bits"),       static_cast<int64>(P.ContentSizeInBits));
				Json.KeyInt (TEXT("total_packet_size_bytes"), static_cast<int64>(P.TotalPacketSizeInBytes));
				Json.KeyInt (TEXT("event_count"),             static_cast<int64>(P.EventCount));
				Json.KeyInt (TEXT("frame_index"),             static_cast<int64>(P.NetProfilerFrameIndex));
				Json.KeyStr (TEXT("delivery"),                FString(DeliveryName(P.DeliveryStatus)));
				Json.KeyStr (TEXT("connection_state"),        FString(ConnectionStateName(P.ConnectionState)));
				Json.EndObject();
				++Emitted;
			});
	}
	Json.EndArray();
	Json.KeyInt (TEXT("emitted"),   Emitted);
	Json.KeyBool(TEXT("truncated"), Emitted < static_cast<int32>(End - Start + 1));
}

static void EmitObjects(const INetProfilerProvider& Provider, const FArgs& Args, FJsonOut& Json)
{
	const int32 InstanceIdx = Args.GameInstanceId;
	Json.KeyInt(TEXT("game_instance_filter"), static_cast<int64>(InstanceIdx));
	const int32 Limit = Args.Limit > 0 ? Args.Limit : 500;

	// Helper: emit one instance's objects, capped at Limit total.
	int32 Emitted = 0;
	int32 Total   = 0;

	auto EmitOne = [&](uint32 InstIdx)
	{
		Total += static_cast<int32>(Provider.GetObjectCount(InstIdx));
		Provider.ReadObjects(InstIdx, [&](const FNetProfilerObjectInstance& O)
		{
			if (Emitted >= Limit) return;
			Json.BeginObject();
			Json.KeyInt(TEXT("game_instance_index"), static_cast<int64>(InstIdx));
			Json.KeyInt(TEXT("object_index"),        static_cast<int64>(O.ObjectIndex));
			Json.KeyInt(TEXT("net_object_id"),       static_cast<int64>(O.NetObjectId));
			Json.KeyInt(TEXT("type_id"),             static_cast<int64>(O.TypeId));
			Json.KeyInt(TEXT("name_index"),          static_cast<int64>(O.NameIndex));
			Json.KeyNum(TEXT("life_begin_ms"),       O.LifeTime.Begin * 1000.0);
			Json.KeyNum(TEXT("life_end_ms"),         FMath::IsFinite(O.LifeTime.End) ? O.LifeTime.End * 1000.0 : -1.0);
			Json.EndObject();
			++Emitted;
		});
	};

	Json.Key(TEXT("events"));
	Json.BeginArray();
	if (InstanceIdx < 0)
	{
		const uint32 N = Provider.GetGameInstanceCount();
		for (uint32 i = 0; i < N; ++i) EmitOne(i);
	}
	else
	{
		EmitOne(static_cast<uint32>(InstanceIdx));
	}
	Json.EndArray();
	Json.KeyInt (TEXT("total_in_window"), Total);
	Json.KeyBool(TEXT("truncated"),       Emitted < Total);
}

void Modes::RunNet(const IAnalysisSession& Session, const FArgs& Args, FJsonOut& Json)
{
	FAnalysisSessionReadScope Lock(Session);
	const INetProfilerProvider* Provider = ReadNetProfilerProvider(Session);
	const FString View = Args.View.IsEmpty() ? FString(TEXT("instances")) : Args.View;

	Json.BeginObject();
	Json.KeyStr(TEXT("file"), Args.File);
	Json.KeyStr(TEXT("mode"), FArgs::ModeName(Args.Mode));
	Json.KeyStr(TEXT("view"), View);

	if (!Provider || Provider->GetNetTraceVersion() == 0)
	{
		// Net version 0 = no network trace data — explicit signal so the
		// caller can tell "channel disabled" from "channel enabled, no
		// traffic yet".
		Json.KeyBool(TEXT("has_net_data"), false);
		Json.KeyInt (TEXT("net_trace_version"), 0);
		Json.Key(TEXT("events")); Json.BeginArray(); Json.EndArray();
		Json.EndObject();
		return;
	}
	Json.KeyBool(TEXT("has_net_data"),      true);
	Json.KeyInt (TEXT("net_trace_version"), static_cast<int64>(Provider->GetNetTraceVersion()));

	if (View.Equals(TEXT("connections"), ESearchCase::IgnoreCase))
	{
		EmitConnections(*Provider, Args, Json);
	}
	else if (View.Equals(TEXT("packets"), ESearchCase::IgnoreCase))
	{
		EmitPackets(*Provider, Args, Json);
	}
	else if (View.Equals(TEXT("objects"), ESearchCase::IgnoreCase))
	{
		EmitObjects(*Provider, Args, Json);
	}
	else  // default: instances
	{
		EmitInstances(*Provider, Json);
	}

	Json.EndObject();
}

} // namespace TraceDigest
