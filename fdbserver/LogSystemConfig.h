/*
 * LogSystemConfig.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBSERVER_LOGSYSTEMCONFIG_H
#define FDBSERVER_LOGSYSTEMCONFIG_H
#pragma once

#include "fdbclient/DatabaseConfiguration.h"
#include "fdbrpc/ReplicationPolicy.h"
#include "fdbserver/BackupInterface.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/TLogInterface.h"
#include "fdbserver/ptxn/TLogInterface.h"

template <class Interface>
struct OptionalInterface {
	friend struct serializable_traits<OptionalInterface<Interface>>;
	// Represents an interface with a known id() and possibly known actual endpoints.
	// For example, an OptionalInterface<TLogInterface> represents a particular tlog by id, which you might or might not
	// presently know how to communicate with

	UID id() const { return ident; }
	bool present() const { return iface.present(); }
	Interface const& interf() const { return iface.get(); }

	explicit OptionalInterface(UID id) : ident(id) {}
	explicit OptionalInterface(Interface const& i) : ident(i.id()), iface(i) {}
	OptionalInterface() {}

	std::string toString() const { return ident.toString(); }

	bool operator==(UID const& r) const { return ident == r; }

	template <class Ar>
	void serialize(Ar& ar);

protected:
	UID ident;
	Optional<Interface> iface;
};

template <class Interface>
template <class Ar>
void OptionalInterface<Interface>::serialize(Ar& ar) {
	serializer(ar, iface);
	if (!iface.present())
		serializer(ar, ident);
	else
		ident = iface.get().id();
}

struct LogSet;
struct OldLogData;

template <class Interface>
struct serializable_traits<OptionalInterface<Interface>> : std::true_type {
	template <class Archiver>
	static void serialize(Archiver& ar, OptionalInterface<Interface>& m) {
		if constexpr (!Archiver::isDeserializing) {
			if (m.iface.present()) {
				m.ident = m.iface.get().id();
			}
		}
		::serializer(ar, m.iface, m.ident);
		if constexpr (Archiver::isDeserializing) {
			if (m.iface.present()) {
				m.ident = m.iface.get().id();
			}
		}
	}
};

// Contains a generation of tLogs for an individual DC.
struct TLogSet {
	constexpr static FileIdentifier file_identifier = 6302317;
	std::vector<OptionalInterface<TLogInterface>> tLogs;

	// TODO: redundant information in "tLogsPtxn" and "ptxnTLogGroups".
	//       We could use map<TLogGroupID, std::vector<int>> instead.

	std::vector<OptionalInterface<ptxn::TLogInterface_PassivelyPull>> tLogsPtxn;
	// A list of TLog Group IDs
	std::vector<ptxn::TLogGroupID> tLogGroupIDs;

	// A list of interface sets, one for each group of "tLogGroupIDs"
	std::vector<std::vector<OptionalInterface<ptxn::TLogInterface_PassivelyPull>>> ptxnTLogGroups;

	std::vector<OptionalInterface<TLogInterface>> logRouters;
	std::vector<OptionalInterface<BackupInterface>> backupWorkers;
	int32_t tLogWriteAntiQuorum, tLogReplicationFactor;
	std::vector<LocalityData> tLogLocalities; // Stores the localities of the log servers
	TLogVersion tLogVersion;
	Reference<IReplicationPolicy> tLogPolicy;
	bool isLocal;
	int8_t locality;
	Version startVersion;
	std::vector<std::vector<int>> satelliteTagLocations;

	TLogSet()
	  : tLogWriteAntiQuorum(0), tLogReplicationFactor(0), isLocal(true), locality(tagLocalityInvalid),
	    startVersion(invalidVersion) {}
	explicit TLogSet(const LogSet& rhs);

	std::string toString() const;

	bool operator==(const TLogSet& rhs) const;

	bool isEqualIds(TLogSet const& r) const;

	template <class Ar>
	void serialize(Ar& ar);
};

template <class Ar>
void TLogSet::serialize(Ar& ar) {
	serializer(ar,
	           tLogs,
	           tLogsPtxn,
	           tLogGroupIDs,
	           ptxnTLogGroups,
	           logRouters,
	           tLogWriteAntiQuorum,
	           tLogReplicationFactor,
	           tLogPolicy,
	           tLogLocalities,
	           isLocal,
	           locality,
	           startVersion,
	           satelliteTagLocations,
	           tLogVersion,
	           backupWorkers);
}

struct OldTLogConf {
	constexpr static FileIdentifier file_identifier = 16233772;
	std::vector<TLogSet> tLogs;
	Version epochBegin, epochEnd;
	int32_t logRouterTags;
	int32_t txsTags;
	std::set<int8_t>
	    pseudoLocalities; // Tracking pseudo localities, e.g., tagLocalityLogRouterMapped, used in the old epoch.
	LogEpoch epoch;

	OldTLogConf() : epochBegin(0), epochEnd(0), logRouterTags(0), txsTags(0), epoch(0) {}
	explicit OldTLogConf(const OldLogData&);

	std::string toString() const {
		return format("end: %d tags: %d %s", epochEnd, logRouterTags, describe(tLogs).c_str());
	}

	bool operator==(const OldTLogConf& rhs) const;

	bool isEqualIds(OldTLogConf const& r) const;

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, tLogs, epochBegin, epochEnd, logRouterTags, pseudoLocalities, txsTags, epoch);
	}
};

enum class LogSystemType {
	// Used as the "0" value of LogSystem
	empty = 0,
	// each TLog is primary for a partition of tags and is secondary for any arbitrary tag.
	tagPartitioned = 2,
	// TLogs are logically partitoned by TLogGroups while each TLogGroup is responsible for
	// a set of storage shard, e.e., key range. Each TLog is a bed of slots for TLogGroups, i.e.,
	// contains an arbitrary number of TLogGroup member.
	teamPartitioned = 3,
	// A fake LogSystem used for testing
	fake = 65535,
	// A fake LogSystem that returns ptxn::test::FakePeekCursor when peeking
	fake_FakePeekCursor
};

struct LogSystemConfig {
	constexpr static FileIdentifier file_identifier = 16360847;
	LogSystemType logSystemType;
	std::vector<TLogSet> tLogs;
	int32_t logRouterTags;
	int32_t txsTags;
	std::vector<OldTLogConf> oldTLogs;
	int32_t expectedLogSets;
	UID recruitmentID;
	bool stopped;
	Optional<Version> recoveredAt;
	std::set<int8_t> pseudoLocalities;
	LogEpoch epoch;
	LogEpoch oldestBackupEpoch;

	LogSystemConfig(LogEpoch e = 0)
	  : logSystemType(LogSystemType::empty), logRouterTags(0), txsTags(0), expectedLogSets(0), stopped(false), epoch(e),
	    oldestBackupEpoch(e) {}

	std::string toString() const;

	Optional<Key> getRemoteDcId() const;

	std::vector<TLogInterface> allLocalLogs(bool includeSatellite = true) const;

	std::vector<TLogInterface> allPresentLogs() const;

	std::pair<int8_t, int8_t> getLocalityForDcId(Optional<Key> dcId) const;

	std::vector<std::pair<UID, NetworkAddress>> allSharedLogs() const;

	bool operator==(const LogSystemConfig& rhs) const { return isEqual(rhs); }

	bool isEqual(LogSystemConfig const& r) const;

	bool isEqualIds(LogSystemConfig const& r) const;

	bool isNextGenerationOf(LogSystemConfig const& r) const;

	bool hasTLog(UID tid) const;

	bool hasLogRouter(UID rid) const;

	bool hasBackupWorker(UID bid) const;

	Version getEpochEndVersion(LogEpoch epoch) const;

	template <class Ar>
	void serialize(Ar& ar);
};

template <class Ar>
void LogSystemConfig::serialize(Ar& ar) {
	serializer(ar,
	           logSystemType,
	           tLogs,
	           logRouterTags,
	           oldTLogs,
	           expectedLogSets,
	           recruitmentID,
	           stopped,
	           recoveredAt,
	           pseudoLocalities,
	           txsTags,
	           epoch,
	           oldestBackupEpoch);
}

#endif // FDBSERVER_LOGSYSTEMCONFIG_H
