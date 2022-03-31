/*
 * TLogPeekCursor.actor.cpp
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

#include "fdbserver/ptxn/TLogPeekCursor.actor.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <vector>

#include "fdbserver/Knobs.h"
#include "fdbserver/ptxn/TLogInterface.h"
#include "fdbserver/ptxn/test/Delay.h"
#include "fdbserver/ptxn/test/Utils.h"
#include "flow/Error.h"
#include "flow/Trace.h"

#include "flow/actorcompiler.h" // This must be the last #include

namespace ptxn {

namespace {

// The deserializer will always expect the serialized data has a header. This function provides header-only serialized
// data for the consumption of the deserializer.
const Standalone<StringRef>& emptyCursorHeader() {
	static Standalone<StringRef> empty;
	if (empty.size() == 0) {
		StorageTeamID storageTeamID;
		ptxn::SubsequencedMessageSerializer serializer(storageTeamID);
		serializer.completeMessageWriting();
		empty = serializer.getSerialized();
	}
	return empty;
};

} // anonymous namespace

namespace details::StorageTeamPeekCursor {

struct PeekRemoteContext {
	const Optional<UID> debugID;
	const StorageTeamID storageTeamID;

	// The last version being processed, the peek will request lastVersion + 1
	Version* pLastVersion;

	// The interface to the remote TLog server
	const std::vector<TLogInterfaceBase*>& pTLogInterfaces;

	// Deserializer
	SubsequencedMessageDeserializer& deserializer;

	// Deserializer iterator
	SubsequencedMessageDeserializer::iterator& wrappedDeserializerIter;

	// Maximum version from the TLog
	Version* pMaxKnownVersion;

	// Minimum version that is known being commited
	Version* pMinKnownCommittedVersion;

	PeekRemoteContext(const Optional<UID>& debugID_,
	                  const StorageTeamID& storageTeamID_,
	                  Version* pLastVersion_,
	                  const std::vector<TLogInterfaceBase*>& pInterfaces_,
	                  SubsequencedMessageDeserializer& deserializer_,
	                  SubsequencedMessageDeserializer::iterator& wrappedDeserializerIterator_,
	                  Version* pMaxKnownVersion_,
	                  Version* pMinKnownCommittedVersion_)
	  : debugID(debugID_), storageTeamID(storageTeamID_), pLastVersion(pLastVersion_), pTLogInterfaces(pInterfaces_),
	    deserializer(deserializer_), wrappedDeserializerIter(wrappedDeserializerIterator_),
	    pMaxKnownVersion(pMaxKnownVersion_), pMinKnownCommittedVersion(pMinKnownCommittedVersion_) {

		for (const auto pTLogInterface : pTLogInterfaces) {
			ASSERT(pTLogInterface != nullptr);
		}
	}
};

ACTOR Future<bool> peekRemote(PeekRemoteContext peekRemoteContext) {
	state TLogPeekRequest request;
	// FIXME: use loadBalancer rather than picking up a random one
	state TLogInterfaceBase* pTLogInterface =
	    peekRemoteContext
	        .pTLogInterfaces[deterministicRandom()->randomInt(0, peekRemoteContext.pTLogInterfaces.size())];

	request.debugID = peekRemoteContext.debugID;
	request.beginVersion = *peekRemoteContext.pLastVersion;
	request.endVersion = invalidVersion; // we *ALWAYS* try to extract *ALL* data
	request.storageTeamID = peekRemoteContext.storageTeamID;

	state TLogPeekReply reply = wait(pTLogInterface->peek.getReply(request));

	// In case the remote epoch ended, an end_of_stream exception will be thrown and it is the caller's responsible
	// to catch.

	peekRemoteContext.deserializer.reset(reply.arena, reply.data);
	peekRemoteContext.wrappedDeserializerIter = peekRemoteContext.deserializer.begin();
	if (peekRemoteContext.wrappedDeserializerIter == peekRemoteContext.deserializer.end()) {
		// No new mutations incoming, and there is no new mutations responded from TLog in this request
		return false;
	}

	*peekRemoteContext.pMaxKnownVersion = reply.maxKnownVersion;
	*peekRemoteContext.pMinKnownCommittedVersion = reply.minKnownCommittedVersion;

	*peekRemoteContext.pLastVersion = reply.endVersion;

	return true;
}

} // namespace details::StorageTeamPeekCursor

#pragma region PeekCursorBase

PeekCursorBase::iterator::iterator(PeekCursorBase* pCursor_, bool isEndIterator_)
  : pCursor(pCursor_), isEndIterator(isEndIterator_) {}

bool PeekCursorBase::iterator::operator==(const PeekCursorBase::iterator& another) const {
	// Since the iterator is not duplicable, no two iterators equal. This is a a hack to help determining if the
	// iterator is reaching the end of the data. See the comments of the constructor.
	return (!pCursor->hasRemaining() && another.isEndIterator && pCursor == another.pCursor);
}

bool PeekCursorBase::iterator::operator!=(const PeekCursorBase::iterator& another) const {
	return !this->operator==(another);
}

PeekCursorBase::iterator::reference PeekCursorBase::iterator::operator*() const {
	return pCursor->get();
}

PeekCursorBase::iterator::pointer PeekCursorBase::iterator::operator->() const {
	return &pCursor->get();
}

void PeekCursorBase::iterator::operator++() {
	pCursor->next();
}

PeekCursorBase::PeekCursorBase() : endIterator(this, true) {}

Future<bool> PeekCursorBase::remoteMoreAvailable() {
	return remoteMoreAvailableImpl();
}

const VersionSubsequenceMessage& PeekCursorBase::get() const {
	return getImpl();
}

void PeekCursorBase::next() {
	nextImpl();
}

void PeekCursorBase::reset() {
	resetImpl();
}

bool PeekCursorBase::hasRemaining() const {
	return hasRemainingImpl();
}

#pragma endregion PeekCursorBase

namespace details {

#pragma region VersionSubsequencePeekCursorBase

VersionSubsequencePeekCursorBase::VersionSubsequencePeekCursorBase() : PeekCursorBase() {}

const Version& VersionSubsequencePeekCursorBase::getVersion() const {
	return get().version;
}

const Subsequence& VersionSubsequencePeekCursorBase::getSubsequence() const {
	return get().subsequence;
}

int VersionSubsequencePeekCursorBase::operatorSpaceship(const VersionSubsequencePeekCursorBase& other) const {
	const Version& thisVersion = getVersion();
	const Version& otherVersion = other.getVersion();
	if (thisVersion < otherVersion) {
		return -1;
	} else if (thisVersion > otherVersion) {
		return 1;
	}

	const Subsequence& thisSubsequence = getSubsequence();
	const Subsequence& otherSubsequence = other.getSubsequence();
	if (thisSubsequence < otherSubsequence) {
		return -1;
	} else if (thisSubsequence > otherSubsequence) {
		return 1;
	}

	return 0;
}

#pragma endregion VersionSubsequencePeekCursorBase

} // namespace details

#pragma region StorageTeamPeekCursor

StorageTeamPeekCursor::StorageTeamPeekCursor(const Version& beginVersion_,
                                             const StorageTeamID& storageTeamID_,
                                             TLogInterfaceBase* pTLogInterface_,
                                             const bool reportEmptyVersion_)
  : StorageTeamPeekCursor(beginVersion_,
                          storageTeamID_,
                          std::vector<TLogInterfaceBase*>{ pTLogInterface_ },
                          reportEmptyVersion_) {}

StorageTeamPeekCursor::StorageTeamPeekCursor(const Version& beginVersion_,
                                             const StorageTeamID& storageTeamID_,
                                             const std::vector<TLogInterfaceBase*>& pTLogInterfaces_,
                                             const bool reportEmptyVersion_)
  : VersionSubsequencePeekCursorBase(), storageTeamID(storageTeamID_), pTLogInterfaces(pTLogInterfaces_),
    deserializer(emptyCursorHeader().arena(), emptyCursorHeader(), /* reportEmptyVersion_ */ true),
    wrappedDeserializerIter(deserializer.begin()), beginVersion(beginVersion_),
    reportEmptyVersion(reportEmptyVersion_), lastVersion(beginVersion_ - 1) {

	for (const auto pTLogInterface : pTLogInterfaces) {
		ASSERT(pTLogInterface != nullptr);
	}
}

const StorageTeamID& StorageTeamPeekCursor::getStorageTeamID() const {
	return storageTeamID;
}

const Version& StorageTeamPeekCursor::getBeginVersion() const {
	return beginVersion;
}

Future<bool> StorageTeamPeekCursor::remoteMoreAvailableImpl() {
	// FIXME Put debugID if necessary
	details::StorageTeamPeekCursor::PeekRemoteContext context(Optional<UID>(),
	                                                          getStorageTeamID(),
	                                                          &lastVersion,
	                                                          pTLogInterfaces,
	                                                          deserializer,
	                                                          wrappedDeserializerIter,
	                                                          &maxKnownVersion,
	                                                          &minKnownCommittedVersion);

	return details::StorageTeamPeekCursor::peekRemote(context);
}

void StorageTeamPeekCursor::nextImpl() {
	++(wrappedDeserializerIter);
}

const VersionSubsequenceMessage& StorageTeamPeekCursor::getImpl() const {
	return *wrappedDeserializerIter;
}

bool StorageTeamPeekCursor::hasRemainingImpl() const {
	if (!reportEmptyVersion) {
		while (wrappedDeserializerIter != deserializer.end() &&
		       wrappedDeserializerIter->message.getType() == Message::Type::EMPTY_VERSION_MESSAGE) {
			++(wrappedDeserializerIter);
		}
	}
	return wrappedDeserializerIter != deserializer.end();
}

void StorageTeamPeekCursor::resetImpl() {
	wrappedDeserializerIter = std::begin(deserializer);
}

#pragma endregion StorageTeamPeekCursor

namespace merged {

namespace details {

std::string CursorContainerBase::toString() const {
	std::stringstream ss;
	ss << "CursorContainerBase: Total = " << size() << std::endl;
	for (const auto& item : container) {
		ss << "\t" << item->getStorageTeamID().toString() << std::endl;
	}
	return ss.str();
}

#pragma region OrderedCursorContainer

bool OrderedCursorContainer::heapElementComparator(OrderedCursorContainer::element_t e1,
                                                   OrderedCursorContainer::element_t e2) {
	return *e1 > *e2;
}

OrderedCursorContainer::OrderedCursorContainer() {
	std::make_heap(std::begin(container), std::end(container), heapElementComparator);
}

void OrderedCursorContainer::pushImpl(const OrderedCursorContainer::element_t& pCursor) {
	container.push_back(pCursor);
	std::push_heap(std::begin(container), std::end(container), heapElementComparator);
}

void OrderedCursorContainer::popImpl() {
	std::pop_heap(std::begin(container), std::end(container), heapElementComparator);
	container.pop_back();
}

void OrderedCursorContainer::eraseImpl(const StorageTeamID& storageTeamID) {
	container.erase(std::remove_if(
	                    std::begin(container),
	                    std::end(container),
	                    [&](const auto& pCursor) -> auto { return pCursor->getStorageTeamID() == storageTeamID; }),
	                std::end(container));
	std::make_heap(std::begin(container), std::end(container), heapElementComparator);
}

#pragma endregion OrderedCursorContainer

#pragma region UnorderedCursorContainer

void UnorderedCursorContainer::pushImpl(const UnorderedCursorContainer::element_t& pCursor) {
	container.push_back(pCursor);
}

void UnorderedCursorContainer::popImpl() {
	container.pop_front();
}

void UnorderedCursorContainer::eraseImpl(const StorageTeamID& storageTeamID) {
	container.erase(std::remove_if(
	                    std::begin(container),
	                    std::end(container),
	                    [&](const auto& pCursor) -> auto { return pCursor->getStorageTeamID() == storageTeamID; }),
	                std::end(container));
}

#pragma endregion UnorderedCursorContainer

#pragma region StorageTeamIDCursorMapper

void StorageTeamIDCursorMapper::addCursor(const std::shared_ptr<StorageTeamPeekCursor>& cursor) {
	addCursorImpl(cursor);
}

void StorageTeamIDCursorMapper::addCursorImpl(const std::shared_ptr<StorageTeamPeekCursor>& cursor) {
	const StorageTeamID& storageTeamID = cursor->getStorageTeamID();
	ASSERT(!isCursorExists(storageTeamID));
	mapper[storageTeamID] = std::shared_ptr<StorageTeamPeekCursor>(cursor);
	storageTeamIDs.insert(storageTeamID);
}

std::shared_ptr<StorageTeamPeekCursor> StorageTeamIDCursorMapper::removeCursor(const StorageTeamID& storageTeamID) {
	ASSERT(isCursorExists(storageTeamID));
	return removeCursorImpl(storageTeamID);
}

std::shared_ptr<StorageTeamPeekCursor> StorageTeamIDCursorMapper::removeCursorImpl(const StorageTeamID& storageTeamID) {
	std::shared_ptr<StorageTeamPeekCursor> result = mapper[storageTeamID];
	storageTeamIDs.erase(storageTeamID);
	mapper.erase(storageTeamID);
	return result;
}

bool StorageTeamIDCursorMapper::isCursorExists(const StorageTeamID& storageTeamID) const {
	return mapper.find(storageTeamID) != mapper.end();
}

StorageTeamPeekCursor& StorageTeamIDCursorMapper::getCursor(const StorageTeamID& storageTeamID) {
	ASSERT(isCursorExists(storageTeamID));
	return *mapper[storageTeamID];
}

const StorageTeamPeekCursor& StorageTeamIDCursorMapper::getCursor(const StorageTeamID& storageTeamID) const {
	ASSERT(isCursorExists(storageTeamID));
	return *mapper.at(storageTeamID);
}

std::shared_ptr<StorageTeamPeekCursor> StorageTeamIDCursorMapper::getCursorPtr(const StorageTeamID& storageTeamID) {
	ASSERT(isCursorExists(storageTeamID));
	return mapper.at(storageTeamID);
}

int StorageTeamIDCursorMapper::getNumCursors() const {
	return mapper.size();
}

StorageTeamIDCursorMapper::StorageTeamIDCursorMapper_t::iterator StorageTeamIDCursorMapper::cursorsBegin() {
	return std::begin(mapper);
}

StorageTeamIDCursorMapper::StorageTeamIDCursorMapper_t::iterator StorageTeamIDCursorMapper::cursorsEnd() {
	return std::end(mapper);
}

#pragma endregion StorageTeamIDCursorMapper

namespace BroadcastedStorageTeamPeekCursor {

struct PeekRemoteContext {
	using GetCursorPtrFunc_t = std::function<std::shared_ptr<StorageTeamPeekCursor>(const StorageTeamID&)>;

	Version* pMaxKnownVersion;
	Version* pMinKnownCommittedVersion;

	// Cursors that are empty
	std::set<StorageTeamID>& emptyCursorStorageTeamIDs;

	// Cursors that meets end-of-stream when querying the TLog server
	std::set<StorageTeamID>& retiredCursorStorageTeamIDs;

	// Function that called to get the corresponding cursor by storage team id
	GetCursorPtrFunc_t getCursorPtr;

	PeekRemoteContext(std::set<StorageTeamID>& emptyCursorStorageTeamIDs_,
	                  std::set<StorageTeamID>& retiredCursorStorageTeamIDs_,
	                  GetCursorPtrFunc_t getCursorPtr_,
	                  Version* pMaxKnownVersion_,
	                  Version* pMinKnownCommittedVersion_)
	  : pMaxKnownVersion(pMaxKnownVersion_), pMinKnownCommittedVersion(pMinKnownCommittedVersion_),
	    emptyCursorStorageTeamIDs(emptyCursorStorageTeamIDs_),
	    retiredCursorStorageTeamIDs(retiredCursorStorageTeamIDs_), getCursorPtr(getCursorPtr_) {}
};

struct PeekSingleCursorResult {
	bool retrievedData = false;
	bool endOfStream = false;
};

ACTOR Future<PeekSingleCursorResult> peekSingleCursor(std::shared_ptr<StorageTeamPeekCursor> pCursor) {
	state int i = 0;
	state ptxn::test::ExponentalBackoffDelay exponentalBackoff(SERVER_KNOBS->MERGE_CURSOR_RETRY_DELAY);
	exponentalBackoff.enable();

	// It is assumed in this scenario, a commit is sent periodically to push the versions of storage servers; so the
	// expoential backup is meaningful.
	while (i < SERVER_KNOBS->MERGE_CURSOR_RETRY_TIMES) {
		try {
			state bool receivedData = wait(pCursor->remoteMoreAvailable());
		} catch (Error& error) {
			if (error.code() == error_code_end_of_stream) {
				return PeekSingleCursorResult{ false, true };
			}
			throw;
		}
		if (receivedData) {
			exponentalBackoff.resetBackoffs();
			return PeekSingleCursorResult{ true, false };
		}
		if (++i == SERVER_KNOBS->MERGE_CURSOR_RETRY_TIMES) {
			break;
		}
		wait(exponentalBackoff());
	}

	return PeekSingleCursorResult{ false, false };
}

// Returns true if all cursors are ready to consume, or any of them timeouts
ACTOR Future<bool> peekRemote(std::shared_ptr<PeekRemoteContext> pPeekRemoteContext) {
	if (pPeekRemoteContext->emptyCursorStorageTeamIDs.empty()) {
		throw end_of_stream();
	}

	state std::vector<std::shared_ptr<StorageTeamPeekCursor>> pCursors;
	std::transform(
	    std::begin(pPeekRemoteContext->emptyCursorStorageTeamIDs),
	    std::end(pPeekRemoteContext->emptyCursorStorageTeamIDs),
	    std::back_inserter(pCursors),
	    /* NOTE ACTORs are compiled into classes */
	    [this](const auto& storageTeamID) -> auto { return pPeekRemoteContext->getCursorPtr(storageTeamID); });

	std::vector<Future<PeekSingleCursorResult>> cursorFutures;
	std::transform(
	    std::begin(pCursors), std::end(pCursors), std::back_inserter(cursorFutures), [](const auto& pCursor) -> auto {
		    return peekSingleCursor(pCursor);
	    });
	state std::vector<PeekSingleCursorResult> cursorResults = wait(getAll(cursorFutures));

	auto cursorResultsIter = std::begin(cursorResults);
	auto emptyCursorStorageTeamIDsIter = std::begin(pPeekRemoteContext->emptyCursorStorageTeamIDs);
	// For any cursors need to be refilled, if the final state is either filled or end_of_stream, the cursors will
	// be ready; otherwise, not ready.
	bool cursorsReady = true;
	while (cursorResultsIter != std::end(cursorResults)) {
		const auto& peekResult = *cursorResultsIter++;
		const StorageTeamID& storageTeamID = *emptyCursorStorageTeamIDsIter;

		// Timeout
		if (!peekResult.endOfStream && !peekResult.retrievedData) {
			TraceEvent(SevWarn, "CursorTimeOutError").detail("StorageTeamID", storageTeamID);
			cursorsReady = false;
			++emptyCursorStorageTeamIDsIter;
			continue;
		}

		if (peekResult.endOfStream) {
			TraceEvent(SevInfo, "CursorEndOfStream").detail("StorageTeamID", storageTeamID);
			UNSTOPPABLE_ASSERT(pPeekRemoteContext->retiredCursorStorageTeamIDs.count(storageTeamID) == 0);
			// NOTE The cursors might be marked retired, yet there could be remaining data.
			pPeekRemoteContext->retiredCursorStorageTeamIDs.insert(storageTeamID);
		}

		pPeekRemoteContext->emptyCursorStorageTeamIDs.erase(emptyCursorStorageTeamIDsIter++);
	}

	if (!cursorsReady) {
		return false;
	}

	// Update other fields depending on TLogPeekReply results
	for (const auto& pCursor : pCursors) {
		*pPeekRemoteContext->pMaxKnownVersion =
		    std::max(*pPeekRemoteContext->pMaxKnownVersion, pCursor->getMaxKnownVersion());
		*pPeekRemoteContext->pMinKnownCommittedVersion =
		    std::max(*pPeekRemoteContext->pMinKnownCommittedVersion, pCursor->getMinKnownCommittedVersion());
	}

	return true;
}

} // namespace BroadcastedStorageTeamPeekCursor

} // namespace details

#pragma region BroadcastedStorageTeamPeekCursorBase

bool BroadcastedStorageTeamPeekCursorBase::tryFillCursorContainer() {
	ASSERT(getNumCursors() != 0);
	ASSERT(pCursorContainer && pCursorContainer->empty());

	const auto lastVersion = currentVersion;
	currentVersion = invalidVersion;
	bool isFirstElement = true;
	int numCursors = 0;
	for (auto iter = cursorsBegin(); iter != cursorsEnd(); ++iter, ++numCursors) {
		auto pCursor = iter->second;
		if (!pCursor->hasRemaining()) {
			emptyCursorStorageTeamIDs.insert(pCursor->getStorageTeamID());
			continue;
		}

		Version cursorVersion = iter->second->getVersion();
		if (isFirstElement) {
			currentVersion = cursorVersion;
			isFirstElement = false;
		} else {
			// In the broadcast model, the cursors must be in state of:
			//   * For cursors that have messages, they share the same version.
			//   * Otherwise, the cursor must have no remaining data, i.e. needs RPC to get refilled.
			// The cursors cannot be lagged behind, or the subsequence constraint cannot be fulfilled.
			UNSTOPPABLE_ASSERT(currentVersion == cursorVersion);
		}
	}

	// The cursor can be empty due to end_of_stream, in this case, remove these cursors from empty cursor set
	std::set<StorageTeamID> retiredAndAllConsumedStorageTeamIDs;
	std::set_intersection(
	    std::begin(emptyCursorStorageTeamIDs),
	    std::end(emptyCursorStorageTeamIDs),
	    std::begin(retiredCursorStorageTeamIDs),
	    std::end(retiredCursorStorageTeamIDs),
	    std::inserter(retiredAndAllConsumedStorageTeamIDs, std::end(retiredAndAllConsumedStorageTeamIDs)));
	std::set<StorageTeamID> notRetiredEmptyCursorStorageTeamIDs;
	std::set_difference(
	    std::begin(emptyCursorStorageTeamIDs),
	    std::end(emptyCursorStorageTeamIDs),
	    std::begin(retiredAndAllConsumedStorageTeamIDs),
	    std::end(retiredAndAllConsumedStorageTeamIDs),
	    std::inserter(notRetiredEmptyCursorStorageTeamIDs, std::end(notRetiredEmptyCursorStorageTeamIDs)));
	std::swap(emptyCursorStorageTeamIDs, notRetiredEmptyCursorStorageTeamIDs);

	// Do we still have storage teams needs RPC call for a refill?
	if (!emptyCursorStorageTeamIDs.empty()) {
		// In case there is only ONE active cursor and the cursor is empty, currentVersion will be set to invalidVersion
		// This would cause any new cursor start with version 0. In this case, we still need to maintain a valid
		// currentVersion. Set it to lastVersion.
		if (numCursors == 1) {
			currentVersion = lastVersion;
		}
		return false;
	}
	// No remaining cursors? Report no more data and let remoteMoreAvailable reports end_of_stream
	if (getNumCursors() == 0 || currentVersion == invalidVersion) {
		return false;
	}

	// Now the cursors are all sharing the same version, fill the cursor container for consumption
	for (auto iter = cursorsBegin(); iter != cursorsEnd(); ++iter) {
		if (emptyCursorStorageTeamIDs.count(iter->first) == 0 &&
		    retiredAndAllConsumedStorageTeamIDs.count(iter->first) == 0) {

			pCursorContainer->push(iter->second.get());
		}
	}

	return true;
}

Future<bool> BroadcastedStorageTeamPeekCursorBase::remoteMoreAvailableImpl() {
	remoteMoreAvailableSnapshot.needSnapshot = true;

	for (const auto& retiredCursorStorageTeamID : retiredCursorStorageTeamIDs) {
		removeCursor(retiredCursorStorageTeamID);
	}
	retiredCursorStorageTeamIDs.clear();

	using PeekRemoteContext = details::BroadcastedStorageTeamPeekCursor::PeekRemoteContext;
	std::shared_ptr<PeekRemoteContext> pContext = std::make_shared<PeekRemoteContext>(
	    emptyCursorStorageTeamIDs,
	    retiredCursorStorageTeamIDs,
	    [this](const StorageTeamID& storageTeamID) -> auto { return getCursorPtr(storageTeamID); },
	    &maxKnownVersion,
	    &minKnownCommittedVersion);
	return details::BroadcastedStorageTeamPeekCursor::peekRemote(pContext);
}

const VersionSubsequenceMessage& BroadcastedStorageTeamPeekCursorBase::getImpl() const {
	return pCursorContainer->front()->get();
}

void BroadcastedStorageTeamPeekCursorBase::addCursorImpl(const std::shared_ptr<StorageTeamPeekCursor>& cursor) {
	ASSERT(!cursor->isEmptyVersionsIgnored());
	// It is possible that a StorageTeamID being inserted for multiple times
	emptyCursorStorageTeamIDs.insert(cursor->getStorageTeamID());
	details::StorageTeamIDCursorMapper::addCursorImpl(cursor);
}

std::shared_ptr<StorageTeamPeekCursor> BroadcastedStorageTeamPeekCursorBase::removeCursorImpl(
    const StorageTeamID& storageTeamID) {

	pCursorContainer->erase(storageTeamID);
	emptyCursorStorageTeamIDs.erase(storageTeamID);
	return details::StorageTeamIDCursorMapper::removeCursorImpl(storageTeamID);
}

bool BroadcastedStorageTeamPeekCursorBase::hasRemainingImpl() const {
	bool tryFillResult = true;
	if (pCursorContainer->empty()) {
		// FIXME Rethink this... const_cast is bitter yet setting hasRemainingImpl non-const is also painful
		tryFillResult = const_cast<BroadcastedStorageTeamPeekCursorBase*>(this)->tryFillCursorContainer();
	}

	// If remoteMoreAvailable is called just before this hasRemainingImpl, a snapshot of internal state is created, in
	// case the cursor is being re-iterated.
	if (remoteMoreAvailableSnapshot.needSnapshot) {
		remoteMoreAvailableSnapshot.needSnapshot = false;
		remoteMoreAvailableSnapshot.version = currentVersion;
		*remoteMoreAvailableSnapshot.pCursorContainer = *pCursorContainer;
	}

	return tryFillResult;
}

void BroadcastedStorageTeamPeekCursorBase::resetImpl() {
	// If the version is invalidVersion, that means we have reached the end of stream
	if (remoteMoreAvailableSnapshot.version == invalidVersion) {
		return;
	}

	currentVersion = remoteMoreAvailableSnapshot.version;
	*pCursorContainer = *remoteMoreAvailableSnapshot.pCursorContainer;

	// The empty cursor will be re-captured during re-iteration; yet the retired cursors are only known during RPC, so
	// those are not reset.
	emptyCursorStorageTeamIDs.clear();

	// Reset all known cursors inside the container to currentVersion
	for (auto cursorIter = cursorsBegin(); cursorIter != cursorsEnd(); ++cursorIter) {
		if (retiredCursorStorageTeamIDs.count(cursorIter->first) > 0) {
			// The cursor is retired, resetting is meaningless
			continue;
		}
		cursorIter->second->reset();
		// Since all data is inside the memory, no RPC call should be called.
		auto vsmIter = cursorIter->second->begin();
		while (vsmIter != cursorIter->second->end() && vsmIter->version < currentVersion) {
			++vsmIter;
		}
	}
}

#pragma endregion BroadcastedStorageTeamPeekCursorBase

void BroadcastedStorageTeamPeekCursor_Ordered::nextImpl() {
	if (pCursorContainer->empty() && !tryFillCursorContainer()) {
		// Calling BroadcastedStorageTeamPeekCursor::next while hasRemaining is false
		// TODO: Rethink this, need better description of this assertion
		ASSERT(false);
	}

	details::OrderedCursorContainer::element_t pConsumedCursor = pCursorContainer->front();
	pCursorContainer->pop();
	pConsumedCursor->next();
	if (pConsumedCursor->hasRemaining() && pConsumedCursor->getVersion() == currentVersion) {
		// The current version is not completely consumed, push it back for consumption
		pCursorContainer->push(pConsumedCursor);
	}
}

void BroadcastedStorageTeamPeekCursor_Unordered::nextImpl() {
	if (pCursorContainer->empty() && !tryFillCursorContainer()) {
		// Calling BroadcastedStorageTeamPeekCursor::next while hasRemaining is false
		ASSERT(false);
	}

	details::OrderedCursorContainer::element_t pConsumedCursor = pCursorContainer->front();
	pConsumedCursor->next();
	if (!pConsumedCursor->hasRemaining() || pConsumedCursor->getVersion() != currentVersion) {
		pCursorContainer->pop();
	}
}

} // namespace merged

// Moves the cursor so it locates to the given version/subsequence. If the version/subsequence does not exist, moves the
// cursor to the closest next mutation. If the version/subsequence is earlier than the current version/subsequence the
// cursor is located, then the code will do nothing.
ACTOR Future<Void> advanceTo(PeekCursorBase* cursor, Version version, Subsequence subsequence) {
	state PeekCursorBase::iterator iter = cursor->begin();

	loop {
		while (iter != cursor->end()) {
			// Is iter already past the version?
			if (iter->version > version) {
				return Void();
			}
			// Is iter current at the given version?
			if (iter->version == version) {
				while (iter != cursor->end() && iter->version == version && iter->subsequence < subsequence)
					++iter;
				if (iter->version > version || iter->subsequence >= subsequence) {
					return Void();
				}
			}
			++iter;
		}

		// Consumed local data, need to check remote TLog
		bool remoteAvailable = wait(cursor->remoteMoreAvailable());
		if (!remoteAvailable) {
			// The version/subsequence should be in the future
			// Throw error?
			return Void();
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////
// ServerPeekCursor used for demo
//////////////////////////////////////////////////////////////////////////////////

ServerPeekCursor::ServerPeekCursor(Reference<AsyncVar<OptionalInterface<TLogInterface_PassivelyPull>>> interf,
                                   Tag tag,
                                   StorageTeamID storageTeamId,
                                   Version begin,
                                   Version end,
                                   bool returnIfBlocked,
                                   bool parallelGetMore)
  : interf(interf), tag(tag), storageTeamId(storageTeamId),
    results(Optional<UID>(), emptyCursorHeader().arena(), emptyCursorHeader()),
    rd(results.arena, results.data, IncludeVersion(ProtocolVersion::withPartitionTransaction())), messageVersion(begin),
    end(end), dbgid(deterministicRandom()->randomUniqueID()), returnIfBlocked(returnIfBlocked),
    parallelGetMore(parallelGetMore) {
	this->results.maxKnownVersion = 0;
	this->results.minKnownCommittedVersion = 0;
	TraceEvent(SevDebug, "SPC_Starting", dbgid)
	    .detail("Team", storageTeamId)
	    .detail("Tag", tag)
	    .detail("Begin", begin)
	    .detail("End", end);
}

ServerPeekCursor::ServerPeekCursor(TLogPeekReply const& results,
                                   LogMessageVersion const& messageVersion,
                                   LogMessageVersion const& end,
                                   TagsAndMessage const& message,
                                   bool hasMsg,
                                   Version poppedVersion,
                                   Tag tag,
                                   StorageTeamID storageTeamId)
  : tag(tag), storageTeamId(storageTeamId), results(results),
    rd(results.arena, results.data, IncludeVersion(ProtocolVersion::withPartitionTransaction())),
    messageVersion(messageVersion), end(end), poppedVersion(poppedVersion), messageAndTags(message), hasMsg(hasMsg),
    dbgid(deterministicRandom()->randomUniqueID()) {
	TraceEvent(SevDebug, "SPC_Clone", dbgid);
	this->results.maxKnownVersion = 0;
	this->results.minKnownCommittedVersion = 0;

	// Consume the message header
	details::MessageHeader messageHeader;
	rd >> messageHeader;

	if (hasMsg)
		nextMessage();

	advanceTo(messageVersion);
}

Reference<ILogSystem::IPeekCursor> ServerPeekCursor::cloneNoMore() {
	return makeReference<ServerPeekCursor>(
	    results, messageVersion, end, messageAndTags, hasMsg, poppedVersion, tag, storageTeamId);
}

void ServerPeekCursor::setProtocolVersion(ProtocolVersion version) {
	rd.setProtocolVersion(version);
}

Arena& ServerPeekCursor::arena() {
	return results.arena;
}

ArenaReader* ServerPeekCursor::reader() {
	return &rd;
}

bool ServerPeekCursor::hasMessage() const {
	TraceEvent(SevDebug, "SPC_HasMessage", dbgid).detail("HasMsg", hasMsg);
	return hasMsg;
}

void ServerPeekCursor::nextMessage() {
	TraceEvent(SevDebug, "SPC_NextMessage", dbgid).detail("MessageVersion", messageVersion.toString());
	ASSERT(hasMsg);
	if (rd.empty()) {
		messageVersion.reset(std::min(results.endVersion, end.version));
		hasMsg = false;
		return;
	}
	if (messageIndexInCurrentVersion >= numMessagesInCurrentVersion) {
		// Read the version header
		while (!rd.empty()) {
			details::SubsequencedItemsHeader sih;
			rd >> sih;
			if (sih.version >= end.version) {
				messageVersion.reset(sih.version);
				hasMsg = false;
				numMessagesInCurrentVersion = 0;
				messageIndexInCurrentVersion = 0;
				return;
			}
			messageVersion.reset(sih.version);
			hasMsg = sih.numItems > 0;
			numMessagesInCurrentVersion = sih.numItems;
			messageIndexInCurrentVersion = 0;
			if (hasMsg) {
				break;
			}
		}
		if (rd.empty()) {
			return;
		}
	}

	Subsequence subsequence;
	rd >> subsequence;
	messageVersion.sub = subsequence;
	hasMsg = true;
	++messageIndexInCurrentVersion;

	// StorageServer.actor.cpp will directly read message from ArenaReader.
	TraceEvent(SevDebug, "SPC_NextMessageB", dbgid).detail("MessageVersion", messageVersion.toString());
}

StringRef ServerPeekCursor::getMessage() {
	// This is not supported yet
	ASSERT(false);
	TraceEvent(SevDebug, "SPC_GetMessage", dbgid);
	StringRef message = messageAndTags.getMessageWithoutTags();
	rd.readBytes(message.size()); // Consumes the message.
	return message;
}

StringRef ServerPeekCursor::getMessageWithTags() {
	ASSERT(false);
	StringRef rawMessage = messageAndTags.getRawMessage();
	rd.readBytes(rawMessage.size() - messageAndTags.getHeaderSize()); // Consumes the message.
	return rawMessage;
}

VectorRef<Tag> ServerPeekCursor::getTags() const {
	ASSERT(false);
	return messageAndTags.tags;
}

void ServerPeekCursor::advanceTo(LogMessageVersion n) {
	TraceEvent(SevDebug, "SPC_AdvanceTo", dbgid).detail("N", n.toString());
	while (messageVersion < n && hasMessage()) {
		if (LogProtocolMessage::isNextIn(rd)) {
			LogProtocolMessage lpm;
			rd >> lpm;
			setProtocolVersion(rd.protocolVersion());
		} else if (rd.protocolVersion().hasSpanContext() && SpanContextMessage::isNextIn(rd)) {
			SpanContextMessage scm;
			rd >> scm;
		} else {
			MutationRef msg;
			rd >> msg;
		}
		nextMessage();
	}

	if (hasMessage())
		return;

	// if( more.isValid() && !more.isReady() ) more.cancel();

	if (messageVersion < n) {
		messageVersion = n;
	}
}

ACTOR Future<Void> resetChecker(ServerPeekCursor* self, NetworkAddress addr) {
	self->slowReplies = 0;
	self->unknownReplies = 0;
	self->fastReplies = 0;
	wait(delay(SERVER_KNOBS->PEEK_STATS_INTERVAL));
	TraceEvent("SlowPeekStats", self->dbgid)
	    .detail("PeerAddress", addr)
	    .detail("SlowReplies", self->slowReplies)
	    .detail("FastReplies", self->fastReplies)
	    .detail("UnknownReplies", self->unknownReplies);

	if (self->slowReplies >= SERVER_KNOBS->PEEK_STATS_SLOW_AMOUNT &&
	    self->slowReplies / double(self->slowReplies + self->fastReplies) >= SERVER_KNOBS->PEEK_STATS_SLOW_RATIO) {

		TraceEvent("ConnectionResetSlowPeek", self->dbgid)
		    .detail("PeerAddress", addr)
		    .detail("SlowReplies", self->slowReplies)
		    .detail("FastReplies", self->fastReplies)
		    .detail("UnknownReplies", self->unknownReplies);
		FlowTransport::transport().resetConnection(addr);
		self->lastReset = now();
	}
	return Void();
}

ACTOR Future<TLogPeekReply> recordRequestMetrics(ServerPeekCursor* self,
                                                 NetworkAddress addr,
                                                 Future<TLogPeekReply> in) {
	try {
		state double startTime = now();
		TLogPeekReply t = wait(in);
		if (now() - self->lastReset > SERVER_KNOBS->PEEK_RESET_INTERVAL) {
			if (now() - startTime > SERVER_KNOBS->PEEK_MAX_LATENCY) {
				if (t.data.size() >= SERVER_KNOBS->DESIRED_TOTAL_BYTES || SERVER_KNOBS->PEEK_COUNT_SMALL_MESSAGES) {
					if (self->resetCheck.isReady()) {
						self->resetCheck = resetChecker(self, addr);
					}
					self->slowReplies++;
				} else {
					self->unknownReplies++;
				}
			} else {
				self->fastReplies++;
			}
		}
		return t;
	} catch (Error& e) {
		if (e.code() != error_code_broken_promise)
			throw;
		wait(Never()); // never return
		throw internal_error(); // does not happen
	}
}

ACTOR Future<Void> serverPeekParallelGetMore(ServerPeekCursor* self, TaskPriority taskID) {
	// Not supported in DEMO
	ASSERT(false);
	if (!self->interf || self->messageVersion >= self->end) {
		if (self->hasMessage())
			return Void();
		wait(Future<Void>(Never()));
		throw internal_error();
	}

	if (!self->interfaceChanged.isValid()) {
		self->interfaceChanged = self->interf->onChange();
	}

	loop {
		state Version expectedBegin = self->messageVersion.version;
		try {
			if (self->parallelGetMore || self->onlySpilled) {
				while (self->futureResults.size() < SERVER_KNOBS->PARALLEL_GET_MORE_REQUESTS &&
				       self->interf->get().present()) {
					self->futureResults.push_back(recordRequestMetrics(
					    self,
					    self->interf->get().interf().peek.getEndpoint().getPrimaryAddress(),
					    self->interf->get().interf().peek.getReply(TLogPeekRequest(self->dbgid,
					                                                               self->messageVersion.version,
					                                                               Optional<Version>(),
					                                                               self->returnIfBlocked,
					                                                               self->onlySpilled,
					                                                               self->storageTeamId),
					                                               taskID)));
				}
				if (self->sequence == std::numeric_limits<decltype(self->sequence)>::max()) {
					throw operation_obsolete();
				}
			} else if (self->futureResults.size() == 0) {
				return Void();
			}

			if (self->hasMessage())
				return Void();

			choose {
				when(TLogPeekReply res = wait(self->interf->get().present() ? self->futureResults.front() : Never())) {
					if (res.beginVersion.get() != expectedBegin) {
						throw operation_obsolete();
					}
					expectedBegin = res.endVersion;
					self->futureResults.pop_front();
					self->results = res;
					self->onlySpilled = res.onlySpilled;
					if (res.popped.present())
						self->poppedVersion =
						    std::min(std::max(self->poppedVersion, res.popped.get()), self->end.version);
					self->rd = ArenaReader(self->results.arena,
					                       self->results.data,
					                       IncludeVersion(ProtocolVersion::withPartitionTransaction()));
					details::MessageHeader messageHeader;
					self->rd >> messageHeader;
					LogMessageVersion skipSeq = self->messageVersion;
					self->hasMsg = true;
					self->nextMessage();
					self->advanceTo(skipSeq);
					TraceEvent(SevDebug, "SPC_GetMoreB", self->dbgid)
					    .detail("Has", self->hasMessage())
					    .detail("End", res.endVersion)
					    .detail("Popped", res.popped.present() ? res.popped.get() : 0);
					return Void();
				}
				when(wait(self->interfaceChanged)) {
					self->interfaceChanged = self->interf->onChange();
					self->dbgid = deterministicRandom()->randomUniqueID();
					self->sequence = 0;
					self->onlySpilled = false;
					self->futureResults.clear();
				}
			}
		} catch (Error& e) {
			if (e.code() == error_code_end_of_stream) {
				self->end.reset(self->messageVersion.version);
				return Void();
			} else if (e.code() == error_code_timed_out || e.code() == error_code_operation_obsolete) {
				TraceEvent("PeekCursorTimedOut", self->dbgid).error(e);
				// We *should* never get timed_out(), as it means the TLog got stuck while handling a parallel peek,
				// and thus we've likely just wasted 10min.
				// timed_out() is sent by cleanupPeekTrackers as value PEEK_TRACKER_EXPIRATION_TIME
				ASSERT_WE_THINK(e.code() == error_code_operation_obsolete ||
				                SERVER_KNOBS->PEEK_TRACKER_EXPIRATION_TIME < 10);
				self->interfaceChanged = self->interf->onChange();
				self->dbgid = deterministicRandom()->randomUniqueID();
				self->sequence = 0;
				self->futureResults.clear();
			} else {
				throw e;
			}
		}
	}
}

ACTOR Future<Void> serverPeekGetMore(ServerPeekCursor* self, TaskPriority taskID) {
	if (!self->interf || self->messageVersion >= self->end) {
		wait(Future<Void>(Never()));
		throw internal_error();
	}
	try {
		loop choose {
			when(TLogPeekReply res = wait(self->interf->get().present()
			                                  ? brokenPromiseToNever(self->interf->get().interf().peek.getReply(
			                                        TLogPeekRequest(self->dbgid,
			                                                        self->messageVersion.version,
			                                                        Optional<Version>(),
			                                                        self->returnIfBlocked,
			                                                        self->onlySpilled,
			                                                        self->storageTeamId),
			                                        taskID))
			                                  : Never())) {
				self->results = res;
				self->onlySpilled = res.onlySpilled;
				if (res.popped.present())
					self->poppedVersion = std::min(std::max(self->poppedVersion, res.popped.get()), self->end.version);
				self->rd = ArenaReader(self->results.arena,
				                       self->results.data,
				                       IncludeVersion(ProtocolVersion::withPartitionTransaction()));
				details::MessageHeader messageHeader;
				self->rd >> messageHeader;
				LogMessageVersion skipSeq = self->messageVersion;
				self->hasMsg = true;
				self->nextMessage();
				self->advanceTo(skipSeq);
				TraceEvent(SevDebug, "SPC_GetMoreB", self->dbgid)
				    .detail("Has", self->hasMessage())
				    .detail("End", res.endVersion)
				    .detail("Popped", res.popped.present() ? res.popped.get() : 0);
				return Void();
			}
			when(wait(self->interf->onChange())) { self->onlySpilled = false; }
		}
	} catch (Error& e) {
		TraceEvent(SevDebug, "SPC_PeekGetMoreError", self->dbgid).error(e, true);
		if (e.code() == error_code_end_of_stream) {
			self->end.reset(self->messageVersion.version);
			return Void();
		}
		throw e;
	}
}

Future<Void> ServerPeekCursor::getMore(TaskPriority taskID) {
	TraceEvent(SevDebug, "SPC_GetMore", dbgid)
	    .detail("More", !more.isValid() || more.isReady())
	    .detail("MessageVersion", messageVersion.toString())
	    .detail("End", end.toString());
	if (hasMessage() && !parallelGetMore)
		return Void();
	if (!more.isValid() || more.isReady()) {
		if (parallelGetMore || onlySpilled || futureResults.size()) {
			ASSERT(false); // not used
			more = serverPeekParallelGetMore(this, taskID);
		} else {
			more = serverPeekGetMore(this, taskID);
		}
	}
	return more;
}

ACTOR Future<Void> serverPeekOnFailed(ServerPeekCursor* self) {
	loop {
		choose {
			when(wait(self->interf->get().present()
			              ? IFailureMonitor::failureMonitor().onStateEqual(
			                    self->interf->get().interf().peek.getEndpoint(), FailureStatus())
			              : Never())) {
				return Void();
			}
			when(wait(self->interf->onChange())) {}
		}
	}
}

Future<Void> ServerPeekCursor::onFailed() {
	return serverPeekOnFailed(this);
}

bool ServerPeekCursor::isActive() const {
	if (!interf->get().present())
		return false;
	if (messageVersion >= end)
		return false;
	return IFailureMonitor::failureMonitor().getState(interf->get().interf().peek.getEndpoint()).isAvailable();
}

bool ServerPeekCursor::isExhausted() const {
	return messageVersion >= end;
}

// Call only after nextMessage(). The sequence of the current message, or
// results.end if nextMessage() has returned false.
const LogMessageVersion& ServerPeekCursor::version() const {
	return messageVersion;
}

Version ServerPeekCursor::getMinKnownCommittedVersion() const {
	return results.minKnownCommittedVersion;
}

Optional<UID> ServerPeekCursor::getPrimaryPeekLocation() const {
	if (interf && interf->get().present()) {
		return interf->get().id();
	}
	return Optional<UID>();
}

Optional<UID> ServerPeekCursor::getCurrentPeekLocation() const {
	return ServerPeekCursor::getPrimaryPeekLocation();
}

Version ServerPeekCursor::popped() const {
	return poppedVersion;
}

//////////////////////////////////////////////////////////////////////////////////
// ServerPeekCursor used for demo -- end
//////////////////////////////////////////////////////////////////////////////////

} // namespace ptxn
