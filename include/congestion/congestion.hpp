// Congestion Observatory - umbrella header.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#ifndef CONGESTION_CONGESTION_HPP
#define CONGESTION_CONGESTION_HPP

#include "congestion/assessment/classify.hpp"
#include "congestion/assessment/policy.hpp"
#include "congestion/correlation/group.hpp"
#include "congestion/core/cancel.hpp"
#include "congestion/core/checked.hpp"
#include "congestion/core/crc64.hpp"
#include "congestion/core/hash.hpp"
#include "congestion/core/json.hpp"
#include "congestion/core/limits.hpp"
#include "congestion/core/name.hpp"
#include "congestion/core/result.hpp"
#include "congestion/core/strong_id.hpp"
#include "congestion/core/time.hpp"
#include "congestion/episode/episode.hpp"
#include "congestion/evidence/evidence.hpp"
#include "congestion/evidence/store.hpp"
#include "congestion/localization/graph.hpp"
#include "congestion/localization/localize.hpp"
#include "congestion/model/generation.hpp"
#include "congestion/model/identities.hpp"
#include "congestion/model/topology.hpp"
#include "congestion/persistence/snapshot.hpp"
#include "congestion/persistence/store.hpp"
#include "congestion/runtime/runtime.hpp"
#include "congestion/transport/client.hpp"
#include "congestion/transport/frame.hpp"
#include "congestion/transport/server.hpp"
#include "congestion/transport/socket.hpp"
#include "congestion/version.hpp"

#endif  // CONGESTION_CONGESTION_HPP
