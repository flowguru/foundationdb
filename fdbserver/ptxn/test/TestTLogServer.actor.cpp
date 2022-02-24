/*
 * TestTLogServer.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2021 Apple Inc. and the FoundationDB project authors
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

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

#include "fdbserver/IDiskQueue.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/ptxn/TLogInterface.h"
#include "fdbserver/ptxn/MessageSerializer.h"
#include "fdbserver/ptxn/test/Driver.h"
#include "fdbserver/ptxn/test/FakeLogSystem.h"
#include "fdbserver/ptxn/test/FakePeekCursor.h"
#include "fdbserver/ptxn/test/Utils.h"
#include "flow/Arena.h"

#include "flow/IRandom.h"
#include "flow/actorcompiler.h" // has to be the last file included

namespace {

// duplicate from worker.actor.cpp, but not seeing a good way to import that code
std::string filenameFromId(KeyValueStoreType storeType, std::string folder, std::string prefix, UID id) {

	if (storeType == KeyValueStoreType::SSD_BTREE_V1)
		return joinPath(folder, prefix + id.toString() + ".fdb");
	else if (storeType == KeyValueStoreType::SSD_BTREE_V2)
		return joinPath(folder, prefix + id.toString() + ".sqlite");
	else if (storeType == KeyValueStoreType::MEMORY || storeType == KeyValueStoreType::MEMORY_RADIXTREE)
		return joinPath(folder, prefix + id.toString() + "-");
	else if (storeType == KeyValueStoreType::SSD_REDWOOD_V1)
		return joinPath(folder, prefix + id.toString() + ".redwood");
	else if (storeType == KeyValueStoreType::SSD_ROCKSDB_V1)
		return joinPath(folder, prefix + id.toString() + ".rocksdb");

	TraceEvent(SevError, "UnknownStoreType").detail("StoreType", storeType);
	UNREACHABLE();
}

ACTOR Future<Void> startTLogServers(std::vector<Future<Void>>* actors,
                                    std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                                    std::string folder,
                                    bool mockDiskQueue = false,
                                    TLogSpillType spillType = TLogSpillType::REFERENCE) {
	state ptxn::test::print::PrintTiming printTiming("startTLogServers");
	state std::vector<ptxn::InitializePtxnTLogRequest> tLogInitializations;
	state Reference<AsyncVar<ServerDBInfo>> dbInfo = makeReference<AsyncVar<ServerDBInfo>>();
	pContext->groupsPerTLog.resize(pContext->numTLogs);
	for (int i = 0, index = 0; i < pContext->numTLogGroups; ++i) {
		ptxn::TLogGroup& tLogGroup = pContext->tLogGroups[i];
		pContext->groupsPerTLog[index].push_back(tLogGroup);
		pContext->groupToLeaderId[tLogGroup.logGroupId] = index;
		++index;
		index %= pContext->numTLogs;
	}
	state int i = 0;
	for (; i < pContext->numTLogs; i++) {
		PromiseStream<ptxn::InitializePtxnTLogRequest> initializeTLog;
		Promise<Void> recovered;
		tLogInitializations.emplace_back();
		tLogInitializations.back().isPrimary = true;
		tLogInitializations.back().storeType = KeyValueStoreType::MEMORY;
		tLogInitializations.back().tlogGroups = pContext->groupsPerTLog[i];
		tLogInitializations.back().spillType = spillType;
		UID tlogId = ptxn::test::randomUID();
		UID workerId = ptxn::test::randomUID();
		StringRef fileVersionedLogDataPrefix = "log2-"_sr;
		StringRef fileLogDataPrefix = "log-"_sr;
		std::string diskQueueFilePrefix = "logqueue-";
		ptxn::InitializePtxnTLogRequest req = tLogInitializations.back();
		const StringRef prefix = req.logVersion > TLogVersion::V2 ? fileVersionedLogDataPrefix : fileLogDataPrefix;

		for (ptxn::TLogGroup& tlogGroup : pContext->groupsPerTLog[i]) {
			std::string filename =
			    filenameFromId(req.storeType, folder, prefix.toString() + "test", tlogGroup.logGroupId);
			IKeyValueStore* data = keyValueStoreMemory(filename, tlogGroup.logGroupId, 500e6);
			IDiskQueue* queue =
			    mockDiskQueue
			        ? new InMemoryDiskQueue(tlogGroup.logGroupId)
			        : openDiskQueue(joinPath(folder, diskQueueFilePrefix + tlogGroup.logGroupId.toString() + "-"),
			                        "fdq",
			                        tlogGroup.logGroupId,
			                        DiskQueueVersion::V1);
			pContext->diskQueues[tlogGroup.logGroupId] = queue;
			pContext->kvStores[tlogGroup.logGroupId] = data;
			tLogInitializations.back().persistentDataAndQueues[tlogGroup.logGroupId] = std::make_pair(data, queue);
		}
		actors->push_back(ptxn::tLog(std::unordered_map<ptxn::TLogGroupID, std::pair<IKeyValueStore*, IDiskQueue*>>(),
		                             dbInfo,
		                             LocalityData(),
		                             initializeTLog,
		                             tlogId,
		                             workerId,
		                             false,
		                             Promise<Void>(),
		                             Promise<Void>(),
		                             folder,
		                             makeReference<AsyncVar<bool>>(false),
		                             makeReference<AsyncVar<UID>>(tlogId)));
		initializeTLog.send(tLogInitializations.back());
		printTiming << "Recruit tlog " << i << " : " << tlogId.shortString() << ", workerID: " << workerId.shortString()
		            << "\n";
	}

	// replace fake TLogInterface with recruited interface
	std::vector<Future<ptxn::TLogInterface_PassivelyPull>> interfaceFutures(pContext->numTLogs);
	for (i = 0; i < pContext->numTLogs; i++) {
		interfaceFutures[i] = tLogInitializations[i].reply.getFuture();
	}
	std::vector<ptxn::TLogInterface_PassivelyPull> interfaces = wait(getAll(interfaceFutures));
	for (i = 0; i < pContext->numTLogs; i++) {
		// This is awkward, but we can't do: *(pContext->tLogInterfaces[i]) = interfaces[i]
		// because this only copies the base class data. The pointer can no longer
		// be casted back to "TLogInterface_PassivelyPull".
		std::shared_ptr<ptxn::TLogInterface_PassivelyPull> tli(new ptxn::TLogInterface_PassivelyPull());
		*tli = interfaces[i];
		pContext->tLogInterfaces[i] = std::static_pointer_cast<ptxn::TLogInterfaceBase>(tli);
	}
	// Update the TLogGroupID to interface mapping
	for (auto& [tLogGroupID, tLogGroupLeader] : pContext->tLogGroupLeaders) {
		tLogGroupLeader = pContext->tLogInterfaces[pContext->groupToLeaderId[tLogGroupID]];
	}
	// Update TLogGroups & TLogInterfaces in ServerDBInfo
	pContext->updateServerDBInfo(dbInfo, interfaces);
	return Void();
}

void generateMutations(const Version& commitVersion,
                       const Version& storageTeamVersion,
                       const int numMutations,
                       const std::vector<ptxn::StorageTeamID>& storageTeamIDs,
                       ptxn::test::CommitRecord& commitRecord) {
	Arena arena;
	VectorRef<MutationRef> mutationRefs;
	ptxn::test::generateMutationRefs(numMutations, arena, mutationRefs);
	ptxn::test::distributeMutationRefs(mutationRefs, commitVersion, storageTeamVersion, storageTeamIDs, commitRecord);
	commitRecord.messageArena.dependsOn(arena);
}

Standalone<StringRef> serializeMutations(const Version& version,
                                         const ptxn::StorageTeamID storageTeamID,
                                         const ptxn::test::CommitRecord& commitRecord) {
	ptxn::ProxySubsequencedMessageSerializer serializer(version);
	for (const auto& [_, message] : commitRecord.messages.at(version).at(storageTeamID)) {
		serializer.write(std::get<MutationRef>(message), storageTeamID);
	};
	auto serialized = serializer.getSerialized(storageTeamID);
	return serialized;
}

const int COMMIT_PEEK_CHECK_MUTATIONS = 20;

// Randomly commit to a tlog, then peek data, and verify if the data is consistent.
ACTOR Future<Void> commitPeekAndCheck(std::shared_ptr<ptxn::test::TestDriverContext> pContext) {
	state ptxn::test::print::PrintTiming printTiming("tlog/commitPeekAndCheck");

	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];
	printTiming << "Storage Team ID: " << storageTeamID.toString() << std::endl;

	state std::shared_ptr<ptxn::TLogInterfaceBase> tli = pContext->getTLogLeaderByStorageTeamID(storageTeamID);
	state Version prevVersion = 0; // starts from 0 for first epoch
	state Version beginVersion = 150;
	state Version endVersion(beginVersion + deterministicRandom()->randomInt(5, 20));
	state Optional<UID> debugID(ptxn::test::randomUID());

	generateMutations(beginVersion,
	                  /* storageTeamVersion = */ 1,
	                  COMMIT_PEEK_CHECK_MUTATIONS,
	                  { storageTeamID },
	                  pContext->commitRecord);
	printTiming << "Generated " << pContext->commitRecord.getNumTotalMessages() << " messages" << std::endl;
	auto serialized = serializeMutations(beginVersion, storageTeamID, pContext->commitRecord);
	std::unordered_map<ptxn::StorageTeamID, StringRef> messages = { { storageTeamID, serialized } };

	// Commit
	ptxn::TLogCommitRequest commitRequest(ptxn::test::randomUID(),
	                                      pContext->storageTeamIDTLogGroupIDMapper[storageTeamID],
	                                      serialized.arena(),
	                                      messages,
	                                      prevVersion,
	                                      beginVersion,
	                                      0,
	                                      0,
	                                      {},
	                                      {},
	                                      std::map<ptxn::StorageTeamID, std::vector<Tag>>(),
	                                      debugID);
	ptxn::test::print::print(commitRequest);

	ptxn::TLogCommitReply commitReply = wait(tli->commit.getReply(commitRequest));
	ptxn::test::print::print(commitReply);

	// Peek
	ptxn::TLogPeekRequest peekRequest(debugID, beginVersion, endVersion, false, false, storageTeamID);
	ptxn::test::print::print(peekRequest);

	ptxn::TLogPeekReply peekReply = wait(tli->peek.getReply(peekRequest));
	ptxn::test::print::print(peekReply);

	// Verify
	ptxn::SubsequencedMessageDeserializer deserializer(peekReply.data);
	ASSERT(storageTeamID == deserializer.getStorageTeamID());
	ASSERT_EQ(beginVersion, deserializer.getFirstVersion());
	ASSERT_EQ(beginVersion, deserializer.getLastVersion());
	int i = 0;
	for (auto iter = deserializer.begin(); iter != deserializer.end(); ++iter, ++i) {
		const ptxn::VersionSubsequenceMessage& m = *iter;
		ASSERT_EQ(beginVersion, m.version);
		ASSERT_EQ(i + 1, m.subsequence); // subsequence starts from 1
		ASSERT(pContext->commitRecord.messages[beginVersion][storageTeamID][i].second ==
		       std::get<MutationRef>(m.message));
	}
	printTiming << "Received " << i << " mutations" << std::endl;
	ASSERT_EQ(i, pContext->commitRecord.messages[beginVersion][storageTeamID].size());

	return Void();
}

ACTOR Future<Void> startStorageServers(std::vector<Future<Void>>* actors,
                                       std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                                       std::string folder) {
	ptxn::test::print::PrintTiming printTiming("testTLogServer/startStorageServers");
	// For demo purpose, each storage server only has one storage team
	ASSERT_EQ(pContext->numStorageServers, pContext->numStorageTeamIDs);
	state std::vector<InitializeStorageRequest> storageInitializations;
	state uint8_t locality = 0; // data center locality

	ServerDBInfo dbInfoBuilder;
	dbInfoBuilder.recoveryState = RecoveryState::ACCEPTING_COMMITS;
	dbInfoBuilder.logSystemConfig.logSystemType = LogSystemType::tagPartitioned;
	dbInfoBuilder.logSystemConfig.tLogs.emplace_back();
	dbInfoBuilder.isTestEnvironment = true;
	auto& tLogSet = dbInfoBuilder.logSystemConfig.tLogs.back();
	tLogSet.locality = locality;

	printTiming << "Assign TLog group leaders" << std::endl;
	for (auto& [groupID, interf] : pContext->tLogGroupLeaders) {
		auto tlogInterf = std::dynamic_pointer_cast<ptxn::TLogInterface_PassivelyPull>(interf);
		ASSERT(tlogInterf != nullptr);
		OptionalInterface<ptxn::TLogInterface_PassivelyPull> optionalInterface =
		    OptionalInterface<ptxn::TLogInterface_PassivelyPull>(*tlogInterf);
		tLogSet.tLogGroupIDs.push_back(groupID);
		tLogSet.ptxnTLogGroups.emplace_back();
		tLogSet.ptxnTLogGroups.back().push_back(optionalInterface);
	}
	state Reference<AsyncVar<ServerDBInfo>> dbInfo = makeReference<AsyncVar<ServerDBInfo>>(dbInfoBuilder);
	state Version tssSeedVersion = 0;
	state int i = 0;
	printTiming << "Recruiting new storage servers" << std::endl;
	UID clusterId = deterministicRandom()->randomUniqueID();
	for (; i < pContext->numStorageServers; i++) {
		pContext->storageServers.emplace_back();
		auto& recruited = pContext->storageServers.back();
		PromiseStream<InitializeStorageRequest> initializeStorage;
		Promise<Void> recovered;
		storageInitializations.emplace_back();

		actors->push_back(storageServer(openKVStore(KeyValueStoreType::StoreType::SSD_BTREE_V2,
		                                            joinPath(folder, "storage-" + recruited.id().toString() + ".ssd-2"),
		                                            recruited.id(),
		                                            0),
		                                recruited,
		                                Tag(locality, i),
		                                clusterId,
		                                tssSeedVersion,
		                                storageInitializations.back().reply,
		                                dbInfo,
		                                folder,
		                                pContext->storageTeamIDs));
		initializeStorage.send(storageInitializations.back());
		printTiming << "Recruited storage server " << i
		            << " : Storage Server Debug ID = " << recruited.id().shortString() << "\n";
	}

	// replace fake Storage Servers with recruited interface
	printTiming << "Updating interfaces" << std::endl;
	std::vector<Future<InitializeStorageReply>> interfaceFutures(pContext->numStorageServers);
	for (i = 0; i < pContext->numStorageServers; i++) {
		interfaceFutures[i] = storageInitializations[i].reply.getFuture();
	}
	std::vector<InitializeStorageReply> interfaces = wait(getAll(interfaceFutures));
	for (i = 0; i < pContext->numStorageServers; i++) {
		pContext->storageServers[i] = interfaces[i].interf;
	}
	return Void();
}

} // anonymous namespace

TEST_CASE("/fdbserver/ptxn/test/run_tlog_server") {
	ptxn::test::TestDriverOptions options(params);
	// Commit validation in real TLog is not supported for now
	options.skipCommitValidation = true;
	state std::vector<Future<Void>> actors;
	state std::vector<Future<Void>> proxies;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);
	// start a real TLog server
	wait(startTLogServers(&actors, pContext, folder));
	// TODO: start fake proxy to talk to real TLog servers.
	startFakeSequencer(actors, pContext);
	startFakeProxy(proxies, pContext);
	wait(waitForAll(proxies));

	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/peek_tlog_server") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);
	// start a real TLog server
	wait(startTLogServers(&actors, pContext, folder));
	wait(commitPeekAndCheck(pContext));

	platform::eraseDirectoryRecursive(folder);
	return Void();
}

namespace {

Version& increaseVersion(Version& version) {
	version += deterministicRandom()->randomInt(5, 10);
	return version;
}

Standalone<StringRef> getLogEntryContent(ptxn::TLogCommitRequest req, UID tlogId) {
	ptxn::TLogQueueEntryRef qe;
	qe.version = req.version;
	// when knownCommittedVersion starts to be changed(now it is 0 constant), here it needs to be changed too
	qe.knownCommittedVersion = 0;
	qe.id = tlogId;
	qe.storageTeams.reserve(req.messages.size());
	qe.messages.reserve(req.messages.size());
	// The structure of a message is:
	//   | Protocol Version | Main Header | Message Header | Message |
	// and sometimes we are only persisting Message Header + Message.
 	const size_t MESSAGE_OVERHEAD_BYTES =
	    ptxn::SerializerVersionOptionBytes + ptxn::getSerializedBytes<ptxn::details::MessageHeader>();

	for (auto& message : req.messages) {
		qe.storageTeams.push_back(message.first);
		qe.messages.push_back(message.second.substr(MESSAGE_OVERHEAD_BYTES));
	}
	BinaryWriter wr(Unversioned()); // outer framing is not versioned
	wr << uint32_t(0);
	IncludeVersion(ProtocolVersion::withTLogQueueEntryRef()).write(wr); // payload is versioned
	wr << qe;
	wr << uint8_t(1);
	*(uint32_t*)wr.getData() = wr.getLength() - sizeof(uint32_t) - sizeof(uint8_t);
	return wr.toValue();
}

ACTOR Future<std::vector<Standalone<StringRef>>> commitInject(std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                                                              ptxn::StorageTeamID storageTeamID,
                                                              int numCommits) {
	state ptxn::test::print::PrintTiming printTiming("tlog/commitInject");

	state const ptxn::TLogGroupID tLogGroupID = pContext->storageTeamIDTLogGroupIDMapper.at(storageTeamID);
	state std::shared_ptr<ptxn::TLogInterfaceBase> pInterface = pContext->getTLogLeaderByStorageTeamID(storageTeamID);
	ASSERT(pInterface);

	state Version currVersion = 0;
	state Version prevVersion = currVersion;
	state Version storageTeamVersion = 0;
	increaseVersion(currVersion);

	state std::vector<ptxn::TLogCommitRequest> requests;
	state std::vector<Standalone<StringRef>> writtenMessages;
	for (auto i = 0; i < numCommits; ++i) {
		generateMutations(currVersion, ++storageTeamVersion, 16, { storageTeamID }, pContext->commitRecord);
		auto serialized = serializeMutations(currVersion, storageTeamID, pContext->commitRecord);
		std::unordered_map<ptxn::StorageTeamID, StringRef> messages = { { storageTeamID, serialized } };
		requests.emplace_back(ptxn::test::randomUID(),
		                      tLogGroupID,
		                      serialized.arena(),
		                      messages,
		                      prevVersion,
		                      currVersion,
		                      0,
		                      0,
		                      std::set<ptxn::StorageTeamID>{},
		                      std::set<ptxn::StorageTeamID>{},
		                      std::map<ptxn::StorageTeamID, std::vector<Tag>>(),
		                      Optional<UID>());
		writtenMessages.emplace_back(getLogEntryContent(requests.back(), pInterface->id()));
		prevVersion = currVersion;
		increaseVersion(currVersion);
	}
	printTiming << "Generated " << numCommits << " commit requests to group " << tLogGroupID.shortString() << std::endl;
	{
		std::mt19937 g(deterministicRandom()->randomUInt32());
		std::shuffle(std::begin(requests), std::end(requests), g);
	}

	state std::vector<Future<ptxn::TLogCommitReply>> replies;
	state int index = 0;
	for (index = 0; index < numCommits; ++index) {
		printTiming << "Sending version " << requests[index].version << std::endl;
		replies.push_back(pInterface->commit.getReply(requests[index]));
		wait(delay(0.5));
	}
	wait(waitForAll(replies));
	printTiming << "Received all replies" << std::endl;

	return writtenMessages;
}

ACTOR Future<Void> pop(std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                       Version version,
                       ptxn::StorageTeamID storageTeamID,
                       Tag tag) {
	ptxn::TLogPopRequest request(version, 0, tag, storageTeamID);
	state std::shared_ptr<ptxn::TLogInterfaceBase> pInterface = pContext->getTLogLeaderByStorageTeamID(storageTeamID);
	wait(pInterface->pop.getReply(request));
	return Void();
}

ACTOR Future<Void> verifyPeek(std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                              ptxn::StorageTeamID storageTeamID,
                              int numCommits) {
	state ptxn::test::print::PrintTiming printTiming("tlog/verifyPeek");

	state std::shared_ptr<ptxn::TLogInterfaceBase> pInterface = pContext->getTLogLeaderByStorageTeamID(storageTeamID);
	ASSERT(pInterface);

	state Version version = 0;

	state int receivedVersions = 0;
	loop {
		ptxn::TLogPeekRequest request(Optional<UID>(), version, 0, false, false, storageTeamID);
		request.endVersion.reset();
		ptxn::TLogPeekReply reply = wait(pInterface->peek.getReply(request));

		ptxn::SubsequencedMessageDeserializer deserializer(reply.data);
		Version v = deserializer.getFirstVersion();

		if (v == invalidVersion) {
			// The TLog has not received committed data, wait and check again
			wait(delay(0.001));
		} else {
			printTiming << concatToString("Received version range [",
			                              deserializer.getFirstVersion(),
			                              ", ",
			                              deserializer.getLastVersion(),
			                              "]")
			            << std::endl;
			std::vector<MutationRef> mutationRefs;
			auto iter = deserializer.begin();
			Arena deserializeArena = iter.arena();
			for (; iter != deserializer.end(); ++iter) {
				const auto& vsm = *iter;
				if (v != vsm.version) {
					printTiming << "Checking version " << v << std::endl;
					ASSERT(pContext->commitRecord.messages.find(v) != pContext->commitRecord.messages.end());
					const auto& recordedMessages = pContext->commitRecord.messages.at(v).at(storageTeamID);
					ASSERT(mutationRefs.size() == recordedMessages.size());
					for (size_t i = 0; i < mutationRefs.size(); ++i) {
						ASSERT(mutationRefs[i] == std::get<MutationRef>(recordedMessages[i].second));
					}

					mutationRefs.clear();
					v = vsm.version;
					++receivedVersions;
				}
				mutationRefs.emplace_back(std::get<MutationRef>(vsm.message));
			}

			{
				printTiming << "Checking version " << v << std::endl;
				const auto& recordedMessages = pContext->commitRecord.messages.at(v).at(storageTeamID);
				ASSERT(mutationRefs.size() == recordedMessages.size());
				for (size_t i = 0; i < mutationRefs.size(); ++i) {
					ASSERT(mutationRefs[i] == std::get<MutationRef>(recordedMessages[i].second));
				}

				++receivedVersions;
			}

			version = deserializer.getLastVersion() + 1;
		}

		if (receivedVersions == numCommits) {
			printTiming << "Over" << std::endl;
			break;
		}
	}

	return Void();
}

// Not officially used since reading from storage server in simulation is not supported yet
#if 0
ACTOR Future<Void> verifyReadStorageServer(std::shared_ptr<ptxn::test::TestDriverContext> pContext,
                                           ptxn::StorageTeamID storageTeamID) {
	state ptxn::test::print::PrintTiming printTiming("storageServer/verifyRead");

	state StorageServerInterface pInterface = pContext->storageServers[0];

	// Get a snapshot of committed Key/Value pairs at the newest version
	state Version latestVersion;
	state std::unordered_map<StringRef, StringRef> keyValues;
	for (const auto& [version, _1] : pContext->commitRecord.messages) {
		latestVersion = version;
		for (const auto& [storageTeamID_, _2] : _1) {
			if (storageTeamID != storageTeamID_) {
				continue;
			}

			for (const auto& [subsequence, message] : _2) {
				ASSERT(message.getType() == ptxn::Message::Type::MUTATION_REF);
				keyValues[std::get<MutationRef>(message).param1] = std::get<MutationRef>(message).param2;
			}
		}
	}

	loop {
		state int tryTimes = 0;
		state bool receivedAllNewValues = true;
		state std::unordered_map<StringRef, StringRef>::iterator iter = std::begin(keyValues);

		while(iter != std::end(keyValues)) {
			state GetValueRequest getValueRequest;
			getValueRequest.key = iter->first;
			getValueRequest.version = latestVersion;

			state GetValueReply getValueReply = wait(pInterface.getValue.getReply(getValueRequest));
			if (!getValueReply.value.present() || getValueReply.value.get() != iter->second) {
				receivedAllNewValues = false;
			}
		}

		if (receivedAllNewValues) {
			break;
		}

		if (++tryTimes == 3) {
			throw internal_error_msg(
			    "After several tries the storage server is still not receiving most recent key value pairs");
		}
		wait(delay(0.1));
	}

	return Void();
}
#endif

} // anonymous namespace

TEST_CASE("/fdbserver/ptxn/test/commit_peek") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 1500e6; // remove after implementing peek from disk
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);

	wait(startTLogServers(&actors, pContext, folder));

	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];
	std::vector<Standalone<StringRef>> messages = wait(commitInject(pContext, storageTeamID, pContext->numCommits));
	wait(verifyPeek(pContext, storageTeamID, pContext->numCommits));
	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/run_storage_server") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 1500e6; // remove after implementing peek from disk
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);
	// start real TLog servers
	wait(startTLogServers(&actors, pContext, folder));

	// Inject data, and verify the read via peek, not cursor

	std::vector<Standalone<StringRef>> messages = wait(commitInject(pContext, pContext->storageTeamIDs[0], 10));
	wait(verifyPeek(pContext, pContext->storageTeamIDs[0], 10));
	// start real storage servers
	wait(startStorageServers(&actors, pContext, folder));

	wait(delay(2.0));

	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/lock_tlog") {
	// idea: 1. lock tlog server first
	//       2. write to a random storage team affiliated to the locked tlog
	//       3. expect tlog_stopped error.

	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	state std::unordered_set<ptxn::TLogGroupID> expectedLockedGroup;
	state std::unordered_set<ptxn::TLogGroupID> groupLocked;
	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);
	// start real TLog servers
	wait(startTLogServers(&actors, pContext, folder));

	// Pick a team, find its group and a tlog
	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];
	auto groupItr = pContext->storageTeamIDTLogGroupIDMapper.find(storageTeamID);
	ASSERT(groupItr != pContext->storageTeamIDTLogGroupIDMapper.end());
	ptxn::TLogGroupID groupId = groupItr->second;

	auto tlogItr = pContext->tLogGroupLeaders.find(groupId);
	ASSERT(tlogItr != pContext->tLogGroupLeaders.end());
	auto tlogInterf = tlogItr->second;

	// Find this tlog interface's index
	auto itr = std::find(pContext->tLogInterfaces.begin(), pContext->tLogInterfaces.end(), tlogInterf);
	ASSERT(itr != pContext->tLogInterfaces.end());
	int index = itr - pContext->tLogInterfaces.begin();
	ASSERT(index < pContext->groupsPerTLog.size());

	// Accumulate expected groups for this tlog
	for (const auto& group : pContext->groupsPerTLog[index]) {
		// insert all groups affiliated to tlog[0] into a expectedSet
		expectedLockedGroup.insert(group.logGroupId);
		ptxn::test::print::print(group);
	}
	ptxn::TLogLockResult result = wait(tlogInterf->lock.getReply<ptxn::TLogLockResult>());
	for (auto& it : result.groupResults) {
		groupLocked.insert(it.id);
	}
	bool allGroupLocked = expectedLockedGroup == groupLocked;
	ASSERT(allGroupLocked);
	ASSERT(!groupLocked.empty()); // at least 1 group belongs to tlog[0]

	state bool tlogStopped = false;
	try {
		std::vector<Standalone<StringRef>> messages = wait(commitInject(pContext, storageTeamID, 1));
	} catch (Error& e) {
		if (e.code() == error_code_tlog_stopped) {
			tlogStopped = true;
		}
	}
	ASSERT(tlogStopped);

	platform::eraseDirectoryRecursive(folder);
	return Void();
}

ACTOR Future<std::pair<std::vector<Standalone<StringRef>>, std::vector<Version>>> commitInjectReturnVersions(
    std::shared_ptr<ptxn::test::TestDriverContext> pContext,
    ptxn::StorageTeamID storageTeamID,
    int numCommits,
    Version cur = 0) {
	state ptxn::test::print::PrintTiming printTiming("tlog/commitInject");

	state const ptxn::TLogGroupID tLogGroupID = pContext->storageTeamIDTLogGroupIDMapper.at(storageTeamID);
	state std::shared_ptr<ptxn::TLogInterfaceBase> pInterface = pContext->getTLogLeaderByStorageTeamID(storageTeamID);
	ASSERT(pInterface);

	state Version currVersion = cur;
	state Version prevVersion = currVersion;
	state Version storageTeamVersion = -1;
	increaseVersion(currVersion);
	increaseVersion(storageTeamVersion);

	state std::vector<ptxn::TLogCommitRequest> requests;
	state std::vector<Standalone<StringRef>> writtenMessages;
	state std::vector<Version> versions;
	for (auto i = 0; i < numCommits; ++i) {
		generateMutations(currVersion, storageTeamVersion, 16, { storageTeamID }, pContext->commitRecord);
		auto serialized = serializeMutations(currVersion, storageTeamID, pContext->commitRecord);
		std::unordered_map<ptxn::StorageTeamID, StringRef> messages = { { storageTeamID, serialized } };
		requests.emplace_back(ptxn::test::randomUID(),
		                      tLogGroupID,
		                      serialized.arena(),
		                      messages,
		                      prevVersion,
		                      currVersion,
		                      0,
		                      0,
		                      std::set<ptxn::StorageTeamID>{},
		                      std::set<ptxn::StorageTeamID>{},
		                      std::map<ptxn::StorageTeamID, std::vector<Tag>>(),
		                      Optional<UID>());
		writtenMessages.emplace_back(getLogEntryContent(requests.back(), pInterface->id()));
		versions.push_back(currVersion);
		prevVersion = currVersion;
		increaseVersion(currVersion);
	}
	printTiming << "Generated " << numCommits << " commit requests to group " << tLogGroupID.shortString() << std::endl;
	{
		std::mt19937 g(deterministicRandom()->randomUInt32());
		std::shuffle(std::begin(requests), std::end(requests), g);
	}

	state std::vector<Future<ptxn::TLogCommitReply>> replies;
	state int index = 0;
	for (index = 0; index < numCommits; ++index) {
		printTiming << "Sending version " << requests[index].version << std::endl;
		replies.push_back(pInterface->commit.getReply(requests[index]));
		wait(delay(0.5));
	}
	wait(waitForAll(replies));
	printTiming << "Received all replies" << std::endl;

	return std::make_pair(writtenMessages, versions);
}

TEST_CASE("/fdbserver/ptxn/test/read_persisted_disk_on_tlog") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL = 0.5;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 1500e6; // remove after implementing peek from disk
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}
	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);

	wait(startTLogServers(&actors, pContext, folder, true));

	state std::pair<std::vector<Standalone<StringRef>>, std::vector<Version>> res =
	    wait(commitInjectReturnVersions(pContext, storageTeamID, pContext->numCommits));
	state std::vector<Standalone<StringRef>> expectedMessages = res.first;
	wait(verifyPeek(pContext, storageTeamID, pContext->numCommits));

	// wait 1s so that actors who update persistent data can do their job.
	wait(delay(1.5));

	// only wrote to a single storageTeamId, thus only 1 tlogGroup, while each tlogGroup has their own disk queue.
	state IDiskQueue* q = pContext->diskQueues[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];
	// in this test, Location must has the same `lo` and `hi`
	// because I did not implement merging multiple location into a single StringRef and return for InMemoryDiskQueue
	ASSERT(q->getNextReadLocation().hi + pContext->numCommits == q->getNextCommitLocation().hi);
	state int commitCnt = 0;

	loop {
		state IDiskQueue::location nextLoc = q->getNextReadLocation();
		state Standalone<StringRef> actual = wait(q->read(nextLoc, nextLoc, CheckHashes::False));
		// Assert contents read are the ones that we previously wrote
		ASSERT(actual.toString() == expectedMessages[commitCnt].toString()); // failed here
		q->pop(nextLoc);
		if (q->getNextReadLocation().hi >= q->getNextCommitLocation().hi) {
			break;
		}
		commitCnt++;
	}

	ASSERT(q->getNextReadLocation() == q->getNextCommitLocation());

	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/pop_data") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL = 0.5;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 1500e6; // disable spilling

	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}
	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];
	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);

	// Spill-by-reference is not finished yet thus test might fail, so using spill-by-value here
	// TODO: have a spill-by-reference test once the support is completed.
	wait(startTLogServers(&actors, pContext, folder, false, TLogSpillType::VALUE));
	state IKeyValueStore* d = pContext->kvStores[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];
	state IDiskQueue* q = pContext->diskQueues[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];

	state std::pair<std::vector<Standalone<StringRef>>, std::vector<Version>> res =
	    wait(commitInjectReturnVersions(pContext, storageTeamID, pContext->numCommits));
	state std::vector<Standalone<StringRef>> expectedMessages = res.first;

	// TODO: uncomment this once enable peek from disk when spill by reference
	// now tests are written in a way assuming spill-by-reference, then verify data is written to disk
	// if spill-by-value, data would not be written to disk.

	// wait(verifyPeek(pContext, storageTeamID, pContext->numCommits));

	ASSERT(q->TEST_getPoppedLocation() == 0);

	wait(pop(pContext,
	         res.second.back(),
	         storageTeamID,
	         pContext->getTLogGroup(pContext->storageTeamIDTLogGroupIDMapper[storageTeamID])
	             .storageTeams[storageTeamID][0]));

	wait(delay(5.0)); // give some time for the updateStorageLoop to run

	int totalSizeExcludeHeader = 0;
	for (const auto& written : res.first) {
		totalSizeExcludeHeader += written.size();
	}
	totalSizeExcludeHeader -= res.first.back().size(); // because poppedLocation record the start location

	// finalPoppedLocation = written messages + page alignment overhead + page headers + spilledData(optional)
	// thus assert on '>'.
	// (note that the last message needs to be excluded because pop location use the start instead of end
	// of a location) ref: https://github.com/apple/foundationdb/blob/4bf14e6/fdbserver/TLogServer.actor.cpp#L919
	ASSERT(q->TEST_getPoppedLocation() > totalSizeExcludeHeader);

	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 1500e6;
	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/read_tlog_spilled") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 0;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL = 0.5;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}
	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);

	wait(startTLogServers(&actors, pContext, folder));
	state IKeyValueStore* d = pContext->kvStores[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];
	state IDiskQueue* q = pContext->diskQueues[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];

	state std::pair<std::vector<Standalone<StringRef>>, std::vector<Version>> res =
	    wait(commitInjectReturnVersions(pContext, storageTeamID, pContext->numCommits));
	state std::vector<Standalone<StringRef>> expectedMessages = res.first;

	// TODO: uncomment this once enable peek from disk when spill by reference
	wait(verifyPeek(pContext, storageTeamID, pContext->numCommits));

	// wait 1s so that actors who update persistent data can do their job.
	wait(delay(1.5));

	// only wrote to a single storageTeamId, thus only 1 tlogGroup, while each tlogGroup has their own disk queue.
	state bool exist = false;
	state int i = 0;

	ASSERT(!res.second.empty());
	// commit to IKeyValueStore might happen in any version of our commits(multiple versions might be combined)
	for (i = 0; i < res.second.size(); i++) {
		state Key k = ptxn::persistStorageTeamMessageRefsKey(
		    pContext->getTLogLeaderByStorageTeamID(storageTeamID)->id(), storageTeamID, res.second[i]);
		state Optional<Value> v = wait(d->readValue(k));
		exist = exist || v.present();
	}

	// we can only assert v is present, because its value is encoded by TLog and it is hard to decode it
	// TODO: assert the value of the spilled data,
	// there are many factors that can change the encoding of the value, such whether it is spilled by value of ref.
	ASSERT(exist);
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 1500e6;
	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/read_tlog_spilled_by_value") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 0;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL = 0.5;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}
	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);

	wait(startTLogServers(&actors, pContext, folder, false, TLogSpillType::VALUE));
	state IKeyValueStore* d = pContext->kvStores[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];
	state IDiskQueue* q = pContext->diskQueues[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];

	state std::pair<std::vector<Standalone<StringRef>>, std::vector<Version>> res =
	    wait(commitInjectReturnVersions(pContext, storageTeamID, pContext->numCommits));
	state std::vector<Standalone<StringRef>> expectedMessages = res.first;

	wait(verifyPeek(pContext, storageTeamID, pContext->numCommits));

	// wait 1s so that actors who update persistent data can do their job.
	wait(delay(1.5));

	// only wrote to a single storageTeamId, thus only 1 tlogGroup, while each tlogGroup has their own disk queue.
	state bool exist = false;
	state int i = 0;

	ASSERT(!res.second.empty());
	// commit to IKeyValueStore might happen in any version of our commits(multiple versions might be combined)

	for (i = 0; i < res.second.size(); i++) {
		state Key k = ptxn::persistStorageTeamMessagesKey(
		    pContext->getTLogLeaderByStorageTeamID(storageTeamID)->id(), storageTeamID, res.second[i]);
		state Optional<Value> v = wait(d->readValue(k));
		exist = exist || v.present();
	}
	// we can only assert v is present, because its value is encoded by TLog and it is hard to decode it
	// there are many factors that can change the encoding of the value, such whether it is spilled by value of ref.
	ASSERT(exist);
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 1500e6;
	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/read_tlog_not_spilled_with_default_threshold") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD =
	    1500e6; // set it as default, in case other tests change it since it is global
	(const_cast<ServerKnobs*> SERVER_KNOBS)->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL = 0.5;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}
	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);

	wait(startTLogServers(&actors, pContext, folder));
	state IKeyValueStore* d = pContext->kvStores[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];

	state std::pair<std::vector<Standalone<StringRef>>, std::vector<Version>> res =
	    wait(commitInjectReturnVersions(pContext, storageTeamID, pContext->numCommits));
	state std::vector<Standalone<StringRef>> expectedMessages = res.first;
	wait(verifyPeek(pContext, storageTeamID, pContext->numCommits));

	// wait 1s so that actors who update persistent data can do their job.
	wait(delay(1.5));

	// only wrote to a single storageTeamId, thus only 1 tlogGroup, while each tlogGroup has their own disk queue.
	state int i = 0;
	ASSERT(!res.second.empty());
	for (i = 0; i < res.second.size(); i++) {
		state Key k = ptxn::persistStorageTeamMessageRefsKey(
		    pContext->getTLogLeaderByStorageTeamID(storageTeamID)->id(), storageTeamID, res.second[i]);
		state Optional<Value> v = wait(d->readValue(k));
		ASSERT(!v.present());
	}

	platform::eraseDirectoryRecursive(folder);
	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/single_tlog_recovery") {
	state ptxn::test::TestDriverOptions options(params);
	state std::vector<Future<Void>> actors;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 0;
	(const_cast<ServerKnobs*> SERVER_KNOBS)->BUGGIFY_TLOG_STORAGE_MIN_UPDATE_INTERVAL = 0.5;
	state std::shared_ptr<ptxn::test::TestDriverContext> pContext = ptxn::test::initTestDriverContext(options);

	for (const auto& group : pContext->tLogGroups) {
		ptxn::test::print::print(group);
	}
	state ptxn::StorageTeamID storageTeamID = pContext->storageTeamIDs[0];

	state std::string folder = "simfdb/" + deterministicRandom()->randomAlphaNumeric(10);
	platform::createDirectory(folder);

	wait(startTLogServers(&actors, pContext, folder, false, TLogSpillType::VALUE));
	state IKeyValueStore* d = pContext->kvStores[pContext->storageTeamIDTLogGroupIDMapper[storageTeamID]];

	state std::pair<std::vector<Standalone<StringRef>>, std::vector<Version>> res =
	    wait(commitInjectReturnVersions(pContext, storageTeamID, pContext->numCommits));

	// TODO: now peek only works with spill-by-value, need another test once spill-by-ref is supported.
	wait(verifyPeek(pContext, storageTeamID, pContext->numCommits));

	// wait here so that actors who update persistentData can do their job.
	wait(delay(1.5));

	// Start to recover, put the same tlog groups in the requests as initial assignment
	state ptxn::TLogGroupID targetGroup = pContext->storageTeamIDTLogGroupIDMapper[storageTeamID];
	state std::unordered_map<ptxn::TLogGroupID, std::pair<IKeyValueStore*, IDiskQueue*>> dqs;
	PromiseStream<ptxn::InitializePtxnTLogRequest> initializeTLogRecover;

	state IDiskQueue::location previousNextPushLocation = pContext->diskQueues[targetGroup]->getNextPushLocation();
	StringRef fileVersionedLogDataPrefix = "log2-"_sr;
	StringRef fileLogDataPrefix = "log-"_sr;
	std::string diskQueueFilePrefix = "logqueue-";

	ptxn::TLogGroup newGroup = ptxn::TLogGroup(ptxn::test::randomUID());
	ptxn::InitializePtxnTLogRequest req;
	req.isPrimary = true;
	req.storeType = KeyValueStoreType::MEMORY;
	req.tlogGroups.push_back(newGroup); // a new group is needed when starting a new tlog, but only for recovery test
	req.recruitmentID = ptxn::test::randomUID(); // need to set recruitementId to avoid caching
	const StringRef prefix = req.logVersion > TLogVersion::V2 ? fileVersionedLogDataPrefix : fileLogDataPrefix;

	std::string oldFilename =
	    filenameFromId(KeyValueStoreType::MEMORY, folder, prefix.toString() + "test", targetGroup);
	IKeyValueStore* data = keyValueStoreMemory(oldFilename, targetGroup, 500e6);
	IDiskQueue* queue = openDiskQueue(
	    joinPath(folder, diskQueueFilePrefix + targetGroup.toString() + "-"), "fdq", targetGroup, DiskQueueVersion::V1);
	dqs[targetGroup] = std::make_pair(data, queue);

	UID newGroupID = deterministicRandom()->randomUniqueID();
	std::string filename = filenameFromId(req.storeType, folder, prefix.toString() + "test", newGroupID);
	req.persistentDataAndQueues[newGroup.logGroupId] =
	    std::make_pair(openKVStore(req.storeType, filename, newGroupID, 500e6),
	                   openDiskQueue(joinPath(folder, diskQueueFilePrefix + newGroupID.toString() + "-"),
	                                 "fdq",
	                                 newGroupID,
	                                 DiskQueueVersion::V1));

	// cancel all actors to shutdown all tlogs, but disk files would not be erase so that we can recover from it.
	for (auto& a : actors) {
		a.cancel();
	}
	actors.clear();
	ASSERT(dqs[targetGroup].second->getNextReadLocation() < previousNextPushLocation);
	// start recovery
	state std::vector<Future<Void>> actors_recover;
	UID tlogId = ptxn::test::randomUID();
	actors_recover.push_back(ptxn::tLog(dqs,
	                                    makeReference<AsyncVar<ServerDBInfo>>(),
	                                    LocalityData(),
	                                    initializeTLogRecover,
	                                    tlogId,
	                                    ptxn::test::randomUID(),
	                                    true,
	                                    Promise<Void>(),
	                                    Promise<Void>(),
	                                    folder,
	                                    makeReference<AsyncVar<bool>>(false),
	                                    makeReference<AsyncVar<UID>>(tlogId)));
	initializeTLogRecover.send(req);

	// wait for the recovery of TLog,
	// cannot read the data and compare bit-by-bit because read operation is only allowed during recovery time.
	wait(success(req.reply.getFuture()));
	wait(delay(5.0)); // give some time for the updateStorageLoop to run

	// From results I see the diff of location::low is always 36(size of DiskQueue::PageHeader)
	// not sure why though, asserting >= would also make sense to me.
	// it is hard to verify through peeking, because the interface is recruited from inside.
	ASSERT(dqs[targetGroup].second->getNextReadLocation() >= previousNextPushLocation);
	ASSERT(dqs[targetGroup].second->getNextReadLocation().lo == previousNextPushLocation.lo + 36);
	(const_cast<ServerKnobs*> SERVER_KNOBS)->TLOG_SPILL_THRESHOLD = 1500e6;

	// TODO: test the old generations interfaces are started and can serve requests such as peek
	platform::eraseDirectoryRecursive(folder);
	return Void();
}
