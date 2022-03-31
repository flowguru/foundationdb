/*
 * TestTLogPeek.actor.cpp
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

#include "fdbserver/ptxn/test/TestTLogPeek.h"

#include <numeric>

#include "fdbserver/ptxn/MessageTypes.h"
#include "fdbserver/ptxn/MutableTeamPeekCursor.actor.h"
#include "fdbserver/ptxn/test/Driver.h"
#include "fdbserver/ptxn/test/FakeTLog.actor.h"
#include "fdbserver/ptxn/test/Utils.h"
#include "fdbserver/ptxn/TLogPeekCursor.actor.h"

#include "flow/actorcompiler.h" // has to be the last file included

namespace ptxn::test {

const int TestTLogPeekOptions::DEFAULT_NUM_VERSIONS = 100;
const int TestTLogPeekOptions::DEFAULT_NUM_MUTATIONS_PER_VERSION = 100;
const int TestTLogPeekOptions::DEFAULT_NUM_TEAMS = 3;
const int TestTLogPeekOptions::DEFAULT_INITIAL_VERSION = 1000;
const int TestTLogPeekOptions::DEFAULT_PEEK_TIMES = 1000;

TestTLogPeekOptions::TestTLogPeekOptions(const UnitTestParameters& params)
  : numVersions(params.getInt("numVersions").orDefault(DEFAULT_NUM_VERSIONS)),
    numMutationsPerVersion(params.getInt("numMutationsPerVersion").orDefault(DEFAULT_NUM_MUTATIONS_PER_VERSION)),
    numStorageTeams(params.getInt("numStorageTeams").orDefault(DEFAULT_NUM_TEAMS)),
    initialVersion(params.getInt("initialVersion").orDefault(DEFAULT_INITIAL_VERSION)),
    peekTimes(params.getInt("peekTimes").orDefault(DEFAULT_PEEK_TIMES)) {}

const int TestTLogPeekMergeCursorOptions::DEFAULT_INITIAL_VERSION = 1000;
const int TestTLogPeekMergeCursorOptions::DEFAULT_NUM_VERSIONS = 10;
const int TestTLogPeekMergeCursorOptions::DEFAULT_NUM_MUTATIONS_PER_VERSION = 100;
const int TestTLogPeekMergeCursorOptions::DEFAULT_NUM_TLOGS = 5;

TestTLogPeekMergeCursorOptions::TestTLogPeekMergeCursorOptions(const UnitTestParameters& params)
  : numTLogs(params.getInt("numTLogs").orDefault(DEFAULT_NUM_TLOGS)),
    numMutationsPerVersion(params.getInt("numMutationsPerVersion").orDefault(DEFAULT_NUM_MUTATIONS_PER_VERSION)),
    initialVersion(params.getInt("initialVersion").orDefault(DEFAULT_INITIAL_VERSION)),
    numVersions(params.getInt("numVersions").orDefault(DEFAULT_NUM_VERSIONS)) {}

// FIXME this should be moved to a more generic place
// Feed the message generated in CommitRecord to TLog servers
ACTOR Future<Void> messageFeeder() {
	state print::PrintTiming printTiming("messageFeeder");
	state RandomDelay randomDelay(0.0, 0.1);
	state CommitRecord::RecordType& committedMessages = TestEnvironment::getCommitRecords().messages;
	randomDelay.enable();

	// Need to use an explicit iterator since there is a wait statement in the loop
	state CommitRecord::RecordType::const_iterator iter;
	iter = std::cbegin(committedMessages);
	state std::vector<Future<TLogCommitReply>> replies;
	for (; iter != std::cend(committedMessages); ++iter) {
		const Version& commitVersion = iter->first;
		const Version& storageTeamVersion =
		    TestEnvironment::getCommitRecords().commitVersionStorageTeamVersionMapper.at(commitVersion);
		printTiming << "Injecting version " << commitVersion << "(Storage Team Version = " << storageTeamVersion
		            << ") to TLogs" << std::endl;

		// Serialize the version
		std::unordered_map<TLogGroupID, std::shared_ptr<ProxySubsequencedMessageSerializer>> tLogGroupSerializers;
		prepareProxySerializedMessages(
		    TestEnvironment::getCommitRecords(),
		    commitVersion,
		    [&tLogGroupSerializers, &storageTeamVersion ](const auto& storageTeamID) -> auto {
			    const auto& mapping = TestEnvironment::getTLogGroup().storageTeamTLogGroupMapping;
			    const auto i = mapping.find(storageTeamID);
			    ASSERT(i != std::end(mapping));
			    const auto tLogGroupID = i->second;
			    if (!tLogGroupSerializers.count(tLogGroupID)) {
				    const auto& storageTeamIDs = TestEnvironment::getTLogGroup().storageTeamIDs;
				    tLogGroupSerializers.emplace(
				        tLogGroupID,
				        std::make_shared<BroadcastedSubsequencedMessageSerializer>(storageTeamVersion, storageTeamIDs));
			    }
			    return tLogGroupSerializers[tLogGroupID];
		    });

		// Send to TLogInterfaces
		for (auto& [tLogGroupID, serializer] : tLogGroupSerializers) {
			auto serialized = serializer->getAllSerialized();
			// NOTE In this test, there is only one storage team per TLog group
			for (const auto& [storageTeamID, serializedData] : serialized.second) {
				printTiming << "TLog Group ID " << tLogGroupID << "  Storage Team ID: " << storageTeamID << std::endl;
				TLogCommitRequest request(deterministicRandom()->randomUniqueID(),
				                          tLogGroupID,
				                          serialized.first,
				                          { { storageTeamID, serializedData } },
				                          0, // FakeTLog does not care previous version yet
				                          commitVersion,
				                          /* knownCommittedVersion */ 0,
				                          /* minKnownCommittedVersion */ 0,
				                          /* addedTeams */ {},
				                          /* removedTeams */ {},
				                          /* teamToTags */ {},
				                          Optional<UID>());
				std::shared_ptr<TLogInterface_PassivelyPull> pInterface =
				    TestEnvironment::getTLogs()->getTLogLeaderByStorageTeamID(storageTeamID);
				replies.push_back(pInterface->commit.getReply(request));
			}
		}

		wait(randomDelay());
	}

	return Void();
}

ACTOR Future<std::vector<VersionSubsequenceMessage>> getAllMessageFromCursor(std::shared_ptr<PeekCursorBase> pCursor,
                                                                             Arena* arena) {
	state std::vector<VersionSubsequenceMessage> messages;
	state std::vector<VersionSubsequenceMessage> messagesDup;
	state RandomDelay randomDelay(0.01, 0.02);
	loop {
		try {
			state bool remoteAvailable = wait(pCursor->remoteMoreAvailable());
		} catch (Error& err) {
			if (err.code() != error_code_end_of_stream) {
				throw;
			}
			break;
		}

		if (!remoteAvailable) {
			// In serious work this should be exponental backoff with jitter
			wait(randomDelay());
		} else {
			auto getAllMessages = [this](std::vector<VersionSubsequenceMessage>& container) {
				for (const VersionSubsequenceMessage& vsm : *pCursor) {
					if (vsm.message.getType() == Message::Type::MUTATION_REF) {
						MutationRef ref = std::get<MutationRef>(vsm.message);
						if (ref.param1.startsWith(storageServerToTeamIdKeyPrefix)) {
							ptxn::StorageServerStorageTeams ss(ref.param2);
						}
					}
					// Empty version message type is not stored in CommitRecord
					if (vsm.message.getType() == Message::Type::EMPTY_VERSION_MESSAGE) {
						continue;
					}
					if (vsm.message.getType() == Message::Type::MUTATION_REF) {
						container.emplace_back(
						    vsm.version, vsm.subsequence, MutationRef(*arena, std::get<MutationRef>(vsm.message)));
					} else {
						container.emplace_back(vsm);
					}
				}
			};

			// Verify the cursor can be repeatedly iterated.
			getAllMessages(messages);
			pCursor->reset();
			getAllMessages(messagesDup);

			ASSERT_EQ(messages.size(), messagesDup.size());
			for (int i = 0; i < static_cast<int>(messages.size()); ++i) {
				ASSERT(messages[i] == messagesDup[i]);
			}
		}
	}
	return messages;
}

} // namespace ptxn::test

TEST_CASE("/fdbserver/ptxn/test/tLogPeek/cursor/StorageTeamPeekCursor") {
	state ptxn::test::TestTLogPeekOptions options(params);
	state ptxn::test::TestEnvironment testEnvironment;
	state ptxn::test::print::PrintTiming printTiming("TestStorageTeamPeekCursor");
	state std::vector<Future<Void>> actors;

	testEnvironment.initDriverContext()
	    .initTLogGroup(1, options.numStorageTeams)
	    .initPtxnTLog(ptxn::MessageTransferModel::StorageServerActivelyPull, 1)
	    .initMessages(options.initialVersion, options.numVersions, options.numMutationsPerVersion);

	for (auto& pTLogContext : ptxn::test::TestEnvironment::getTLogs()->tLogContexts) {
		// Limit the versions per reply, to force multiple peeks
		pTLogContext->maxVersionsPerPeek = deterministicRandom()->randomInt(1, 5);
		pTLogContext->latency.enable();
	}

	// Inject the messages
	actors.push_back(ptxn::test::messageFeeder());

	const auto& storageTeamID = ptxn::test::randomlyPick(ptxn::test::TestEnvironment::getTLogGroup().storageTeamIDs);
	state std::vector<ptxn::VersionSubsequenceMessage> messagesGenerated =
	    ptxn::test::TestEnvironment::getCommitRecords().getMessagesFromStorageTeams({ storageTeamID });

	// Peek from one TLog server
	state Arena messageArena;
	std::shared_ptr<ptxn::TLogInterface_PassivelyPull> pInterface =
	    ptxn::test::TestEnvironment::getTLogs()->getTLogLeaderByStorageTeamID(storageTeamID);

	// FIXME reportEmptyVersion should reflect the value of (SERVER_KNOBS->INSERT_EMPTY_TRANSACTION ||
	// SERVER_KNOBS->BROADCAST_TLOG_GROUPS)
	state std::shared_ptr<ptxn::StorageTeamPeekCursor> pCursor = std::make_shared<ptxn::StorageTeamPeekCursor>(
	    options.initialVersion, storageTeamID, pInterface.get());

	state Arena arena;
	state std::vector<ptxn::VersionSubsequenceMessage> messagesFromTLog =
	    wait(ptxn::test::getAllMessageFromCursor(pCursor, &arena));

	// Verify
	ASSERT_EQ(messagesFromTLog.size(), messagesGenerated.size());
	for (int i = 0; i < static_cast<int>(messagesFromTLog.size()); ++i) {
		ASSERT(messagesFromTLog[i] == messagesGenerated[i]);
	}

	return Void();
}

namespace {

void verifyMergedCursorResult_Ordered(const std::vector<ptxn::VersionSubsequenceMessage>& messagesFromTLogs) {
	auto messagesGenerated = ptxn::test::TestEnvironment::getCommitRecords().getMessagesFromStorageTeams();
	ASSERT_EQ(messagesFromTLogs.size(), messagesGenerated.size());
	for (int i = 0; i < static_cast<int>(messagesFromTLogs.size()); ++i) {
		ASSERT(messagesFromTLogs[i] == messagesGenerated[i]);
	}
}

void verifyMergedCursorResult_Unordered(const std::vector<ptxn::VersionSubsequenceMessage>& messagesFromTLogs) {
	auto& commitRecords = ptxn::test::TestEnvironment::getCommitRecords();
	auto messagesGenerated = commitRecords.getMessagesFromStorageTeams();
	ASSERT_EQ(messagesFromTLogs.size(), messagesGenerated.size());

	Version currentVersion = invalidVersion;
	ptxn::StorageTeamID currentStorageTeamID;
	int storageTeamMessageIndex = 0;
	bool versionStorageTeamTerminated = true;
	for (const auto& vsm : messagesFromTLogs) {
		if (vsm.version != currentVersion) {
			currentVersion = vsm.version;
		}
		const auto& storageTeamMessage = commitRecords.messages.at(currentVersion);

		if (versionStorageTeamTerminated) {
			// Find the storage team for the current message
			bool found = false;
			for (const auto& [storageTeamID, subsequenceMessages] : storageTeamMessage) {
				if (subsequenceMessages.size() == 0) {
					continue;
				}
				if (subsequenceMessages[0].first == vsm.subsequence && subsequenceMessages[0].second == vsm.message) {
					currentStorageTeamID = storageTeamID;
					storageTeamMessageIndex = 0;
					found = true;
					break;
				}
			}
			ASSERT(found);
			versionStorageTeamTerminated = false;
		}

		const auto& messages = storageTeamMessage.at(currentStorageTeamID);
		ASSERT_EQ(messages[storageTeamMessageIndex].first, vsm.subsequence);
		ASSERT(messages[storageTeamMessageIndex].second == vsm.message);

		if (++storageTeamMessageIndex >= messages.size()) {
			versionStorageTeamTerminated = true;
		}
	}
}

ACTOR template <typename CursorType>
Future<Void> runMergedCursorTest(ptxn::test::TestTLogPeekMergeCursorOptions options) {
	state ptxn::test::TestEnvironment testEnvironment;
	state std::vector<Future<Void>> actors;

	testEnvironment.initDriverContext()
	    .initTLogGroup(options.numTLogs, options.numTLogs)
	    .initPtxnTLog(ptxn::MessageTransferModel::StorageServerActivelyPull, options.numTLogs)
	    .initMessages(options.initialVersion, options.numVersions, options.numMutationsPerVersion)
	    .broadcastEmptyVersionMessage();

	ptxn::test::print::printCommitRecords();

	// Force multiple time peek, and peek causes latency
	for (auto pFakeTLogContext : ptxn::test::TestEnvironment::getTLogs()->tLogContexts) {
		pFakeTLogContext->maxVersionsPerPeek = deterministicRandom()->randomInt(3, 5);
		pFakeTLogContext->latency.enable();
	}

	// Inject the commits to TLogs
	actors.push_back(ptxn::test::messageFeeder());

	// Initialize the cursor
	state Arena messageArena;
	state std::shared_ptr<CursorType> mergedCursor = std::make_shared<CursorType>();
	const std::vector<ptxn::StorageTeamID>& storageTeamIDs = ptxn::test::TestEnvironment::getTLogGroup().storageTeamIDs;
	for (const auto& storageTeamID : storageTeamIDs) {
		std::shared_ptr<ptxn::TLogInterface_PassivelyPull> pInterface =
		    ptxn::test::TestEnvironment::getTLogs()->getTLogLeaderByStorageTeamID(storageTeamID);
		mergedCursor->addCursor(std::make_shared<ptxn::StorageTeamPeekCursor>(options.initialVersion,
		                                                                      storageTeamID,
		                                                                      pInterface.get(),
		                                                                      /* reportEmptyVersion = */ true));
	}

	// Query all messages using a merged cursor
	state Arena arena;
	state std::vector<ptxn::VersionSubsequenceMessage> messagesFromTLogs =
	    wait(ptxn::test::getAllMessageFromCursor(mergedCursor, &arena));

	wait(waitForAll(actors));

	// FIXME: Use constexpr when actor compiler supports this
	if (std::is_base_of<ptxn::merged::BroadcastedStorageTeamPeekCursor_Ordered, CursorType>::value) {
		verifyMergedCursorResult_Ordered(messagesFromTLogs);
	} else if (std::is_base_of<ptxn::merged::BroadcastedStorageTeamPeekCursor_Unordered, CursorType>::value) {
		verifyMergedCursorResult_Unordered(messagesFromTLogs);
	} else {
		// Unsupported cursor type
		ASSERT(false);
	}

	return Void();
}

} // anonymous namespace

TEST_CASE("/fdbserver/ptxn/test/tLogPeek/cursor/merged/BroadcastedStorageTeamPeekCursor_Ordered") {
	state ptxn::test::TestTLogPeekMergeCursorOptions options(params);

	wait(runMergedCursorTest<ptxn::merged::BroadcastedStorageTeamPeekCursor_Ordered>(options));

	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/tLogPeek/cursor/merged/BroadcastedStorageTeamPeekCursor_Unordered") {
	state ptxn::test::TestTLogPeekMergeCursorOptions options(params);

	wait(runMergedCursorTest<ptxn::merged::BroadcastedStorageTeamPeekCursor_Unordered>(options));

	return Void();
}

TEST_CASE("/fdbserver/ptxn/test/tLogPeek/cursor/advanceTo") {
	state ptxn::test::TestTLogPeekMergeCursorOptions options(params);
	state ptxn::test::TestEnvironment testEnvironment;
	state ptxn::test::print::PrintTiming printTiming("TestAdvanceTo");
	state std::vector<Future<Void>> actors;

	testEnvironment.initDriverContext()
	    .initTLogGroup(options.numTLogs, /* numStorageTeams */ options.numTLogs * 3)
	    .initPtxnTLog(ptxn::MessageTransferModel::StorageServerActivelyPull, options.numTLogs)
	    .initMessages(options.initialVersion, options.numVersions, options.numMutationsPerVersion)
	    .broadcastEmptyVersionMessage();

	for (auto pFakeTLogContext : ptxn::test::TestEnvironment::getTLogs()->tLogContexts) {
		pFakeTLogContext->latency.enable();
	}

	// Inject the commits to TLogs
	actors.push_back(ptxn::test::messageFeeder());

	// Initialize the cursor
	state Arena messageArena;
	// Unordered cursor cannot correctly advanceTo
	state std::shared_ptr<ptxn::merged::BroadcastedStorageTeamPeekCursor_Ordered> mergedCursor =
	    std::make_shared<ptxn::merged::BroadcastedStorageTeamPeekCursor_Ordered>();
	const std::vector<ptxn::StorageTeamID>& storageTeamIDs = ptxn::test::TestEnvironment::getTLogGroup().storageTeamIDs;
	for (const auto& storageTeamID : storageTeamIDs) {
		std::shared_ptr<ptxn::TLogInterface_PassivelyPull> pInterface =
		    ptxn::test::TestEnvironment::getTLogs()->getTLogLeaderByStorageTeamID(storageTeamID);
		mergedCursor->addCursor(std::make_shared<ptxn::StorageTeamPeekCursor>(options.initialVersion,
		                                                                      storageTeamID,
		                                                                      pInterface.get(),
		                                                                      /* reportEmptyVersion = */ true));
	}

	const auto& commitRecords = ptxn::test::TestEnvironment::getCommitRecords().messages;

	// Find a random version/subsequence/message
	{
		std::vector<Version> versions;
		for (const auto& [version, _] : commitRecords) {
			versions.push_back(version);
		}
		state Version advanceToVersion = ptxn::test::randomlyPick(versions);

		std::vector<ptxn::StorageTeamID> storageTeamIDs;
		for (const auto& [storageTeamID, subsequencedMessages] : commitRecords.at(advanceToVersion)) {
			if (subsequencedMessages.size() == 0) {
				continue;
			}
			storageTeamIDs.push_back(storageTeamID);
		}
		// At least one storage team has non-empty message
		ASSERT(storageTeamIDs.size() > 0);

		state ptxn::StorageTeamID advanceToUseStorageTeamID = ptxn::test::randomlyPick(storageTeamIDs);

		std::vector<Subsequence> subsequences;
		for (const auto& [subsequence, _] : commitRecords.at(advanceToVersion).at(advanceToUseStorageTeamID)) {
			subsequences.push_back(subsequence);
		}
		state Subsequence advanceToSubsequence = ptxn::test::randomlyPick(subsequences);

		printTiming << "Advancing to " << advanceToVersion << ", " << advanceToSubsequence << std::endl;

		wait(ptxn::advanceTo(mergedCursor.get(), advanceToVersion, advanceToSubsequence));
		auto vsm = mergedCursor->get();
		printTiming << "Cursor reached " << vsm.version << ", " << vsm.subsequence << std::endl;
		ASSERT_EQ(vsm.version, advanceToVersion);
		ASSERT_EQ(vsm.subsequence, advanceToSubsequence);
	}

	return Void();
}

namespace {

std::vector<ptxn::TLogInterfaceBase*> getTLogInterfaceByStorageTeamID(const ptxn::StorageTeamID& storageTeamID) {
	return { static_cast<std::shared_ptr<ptxn::TLogInterface_PassivelyPull>>(
		         ptxn::test::TestEnvironment::getTLogs()->getTLogLeaderByStorageTeamID(storageTeamID))
		         .get() };
}

// Collects all messages, with respect to
// NOTE It is assumed that only one storage server is used, i.e. not checking the storage server ID for simplicity
// purpose.
// NOTE It is assumed only one single privateMutationsStorageTeamID exists.
std::vector<ptxn::VersionSubsequenceMessage> getMessagesFromMutableTeams(
    const ptxn::StorageTeamID& privateMutationsStorageTeamID) {

	const auto& commitRecord = ptxn::test::TestEnvironment::getCommitRecords();
	std::vector<ptxn::VersionSubsequenceMessage> result;
	ptxn::StorageServerStorageTeams activeStorageServerTeams(privateMutationsStorageTeamID);

	for (const auto& [commitVersion, storageTeamSubsequenceMessages] : commitRecord.messages) {
		for (const auto& [storageTeamID, subsequenceMessages] : storageTeamSubsequenceMessages) {
			if (subsequenceMessages.size() == 0) {
				continue;
			}

			if (storageTeamID == privateMutationsStorageTeamID) {
				// The team mutation KV pair will always have subsequence 1
				const auto& message = subsequenceMessages[0].second;
				result.emplace_back(commitVersion, subsequenceMessages[0].first, message);
				auto newStorageTeam = ptxn::StorageServerStorageTeams(std::get<MutationRef>(message).param2);
				ASSERT(newStorageTeam.getPrivateMutationsStorageTeamID() == privateMutationsStorageTeamID);
				activeStorageServerTeams = newStorageTeam;
				continue;
			}

			if (!activeStorageServerTeams.contains(storageTeamID)) {
				continue;
			}

			for (const auto& [subsequence, message] : subsequenceMessages) {
				result.emplace_back(commitVersion, subsequence, message);
			}
		}
	}

	std::sort(std::begin(result), std::end(result));

	return result;
}

} // anonymous namespace

TEST_CASE("/fdbserver/ptxn/test/tLogPeek/cursor/merged/OrderedMutableTeamPeekCursor") {
	state ptxn::test::TestTLogPeekMergeCursorOptions options(params);
	state ptxn::test::TestEnvironment testEnvironment;
	state std::vector<Future<Void>> actors;
	state const std::vector<UID> storageServerIDs = { deterministicRandom()->randomUniqueID() };
	state std::shared_ptr<ptxn::merged::OrderedMutableTeamPeekCursor> pCursor;
	state ptxn::test::print::PrintTiming printTiming("testOrderedMutableTeamPeekCursor");

	testEnvironment
	    .initDriverContext()
	    // At this stage we set num tLogs same to numStorageTeams
	    .initTLogGroupWithPrivateMutationsFixture(options.numTLogs, options.numTLogs)
	    .initPtxnTLog(ptxn::MessageTransferModel::StorageServerActivelyPull, options.numTLogs)
	    .initMessagesWithPrivateMutations(options.initialVersion,
	                                      options.numVersions,
	                                      options.numMutationsPerVersion,
	                                      Optional<std::vector<UID>>(storageServerIDs))
	    .broadcastEmptyVersionMessage();

	ptxn::test::print::printCommitRecords();

	// Force multiple time peek, and peek causes latency
	for (auto pFakeTLogContext : ptxn::test::TestEnvironment::getTLogs()->tLogContexts) {
		pFakeTLogContext->maxVersionsPerPeek = 10;
		pFakeTLogContext->latency.enable();
	}

	// Inject the commits to TLogs
	actors.push_back(ptxn::test::messageFeeder());

	state ptxn::StorageTeamID privateMutationsStorageTeamID =
	    dynamic_cast<const ptxn::test::details::TLogGroupWithPrivateMutationsFixture&>(
	        ptxn::test::TestEnvironment::getTLogGroup())
	        .privateMutationsStorageTeamID;

	pCursor = std::make_shared<ptxn::merged::OrderedMutableTeamPeekCursor>(
	    storageServerIDs[0], privateMutationsStorageTeamID, getTLogInterfaceByStorageTeamID);

	state Arena storageArena;
	state std::vector<ptxn::VersionSubsequenceMessage> messagesFromTLogs =
	    wait(ptxn::test::getAllMessageFromCursor(pCursor, &storageArena));

	const std::vector<ptxn::VersionSubsequenceMessage> messagesFromCommitRecord =
	    getMessagesFromMutableTeams(privateMutationsStorageTeamID);

	ASSERT_EQ(messagesFromTLogs.size(), messagesFromCommitRecord.size());
	for (int i = 0; i < static_cast<int>(messagesFromTLogs.size()); ++i) {
		ASSERT(messagesFromTLogs[i] == messagesFromCommitRecord[i]);
	}

	return Void();
}