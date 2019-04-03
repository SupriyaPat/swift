//===--- DiagnosticConsumer.cpp - Diagnostic Consumer Impl ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the DiagnosticConsumer class.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "swift-ast"
#include "swift/AST/DiagnosticConsumer.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;

DiagnosticConsumer::~DiagnosticConsumer() = default;

llvm::SMLoc DiagnosticConsumer::getRawLoc(SourceLoc loc) {
  return loc.Value;
}

LLVM_ATTRIBUTE_UNUSED
static bool hasDuplicateFileNames(
    ArrayRef<FileSpecificDiagnosticConsumer::Subconsumer> subconsumers) {
  llvm::StringSet<> seenFiles;
  for (const auto &subconsumer : subconsumers) {
    if (subconsumer.getInputFileName().empty()) {
      // We can handle multiple subconsumers that aren't associated with any
      // file, because they only collect diagnostics that aren't in any of the
      // special files. This isn't an important use case to support, but also
      // SmallSet doesn't handle empty strings anyway!
      continue;
    }

    bool isUnique = seenFiles.insert(subconsumer.getInputFileName()).second;
    if (!isUnique)
      return true;
  }
  return false;
}

std::unique_ptr<DiagnosticConsumer>
FileSpecificDiagnosticConsumer::consolidateSubconsumers(
    SmallVectorImpl<Subconsumer> &subconsumers) {
  if (subconsumers.empty())
    return nullptr;
  if (subconsumers.size() == 1)
    return std::move(subconsumers.front()).consumer;
  // Cannot use return
  // llvm::make_unique<FileSpecificDiagnosticConsumer>(subconsumers); because
  // the constructor is private.
  return std::unique_ptr<DiagnosticConsumer>(
      new FileSpecificDiagnosticConsumer(subconsumers));
}

FileSpecificDiagnosticConsumer::FileSpecificDiagnosticConsumer(
    SmallVectorImpl<Subconsumer> &subconsumers)
    : Subconsumers(std::move(subconsumers)) {
  assert(!Subconsumers.empty() &&
         "don't waste time handling diagnostics that will never get emitted");
  assert(!hasDuplicateFileNames(Subconsumers) &&
         "having multiple subconsumers for the same file is not implemented");
}

void FileSpecificDiagnosticConsumer::computeConsumersOrderedByRange(
    SourceManager &SM) {
  // Look up each file's source range and add it to the "map" (to be sorted).
  for (const unsigned subconsumerIndex: indices(Subconsumers)) {
    const Subconsumer &subconsumer = Subconsumers[subconsumerIndex];
    if (subconsumer.getInputFileName().empty())
      continue;

    Optional<unsigned> bufferID =
        SM.getIDForBufferIdentifier(subconsumer.getInputFileName());
    assert(bufferID.hasValue() && "consumer registered for unknown file");
    CharSourceRange range = SM.getRangeForBuffer(bufferID.getValue());
    ConsumersOrderedByRange.emplace_back(
        ConsumerAndRange(range, subconsumerIndex));
  }

  // Sort the "map" by buffer /end/ location, for use with std::lower_bound
  // later. (Sorting by start location would produce the same sort, since the
  // ranges must not be overlapping, but since we need to check end locations
  // later it's consistent to sort by that here.)
  std::sort(ConsumersOrderedByRange.begin(), ConsumersOrderedByRange.end());

  // Check that the ranges are non-overlapping. If the files really are all
  // distinct, this should be trivially true, but if it's ever not we might end
  // up mis-filing diagnostics.
  assert(ConsumersOrderedByRange.end() ==
             std::adjacent_find(ConsumersOrderedByRange.begin(),
                                ConsumersOrderedByRange.end(),
                                [](const ConsumerAndRange &left,
                                   const ConsumerAndRange &right) {
                                  return left.overlaps(right);
                                }) &&
         "overlapping ranges despite having distinct files");
}

Optional<FileSpecificDiagnosticConsumer::Subconsumer *>
FileSpecificDiagnosticConsumer::subconsumerForLocation(SourceManager &SM,
                                                       SourceLoc loc) {
  // Diagnostics with invalid locations always go to every consumer.
  if (loc.isInvalid())
    return None;

  // This map is generated on first use and cached, to allow the
  // FileSpecificDiagnosticConsumer to be set up before the source files are
  // actually loaded.
  if (ConsumersOrderedByRange.empty()) {

    // It's possible to get here while a bridging header PCH is being
    // attached-to, if there's some sort of AST-reader warning or error, which
    // happens before CompilerInstance::setUpInputs(), at which point _no_
    // source buffers are loaded in yet. In that case we return None, rather
    // than trying to build a nonsensical map (and actually crashing since we
    // can't find buffers for the inputs).
    assert(!Subconsumers.empty());
    if (!SM.getIDForBufferIdentifier(Subconsumers.begin()->getInputFileName())
             .hasValue()) {
      assert(llvm::none_of(Subconsumers, [&](const Subconsumer &subconsumer) {
        return SM.getIDForBufferIdentifier(subconsumer.getInputFileName())
            .hasValue();
      }));
      return None;
    }
    auto *mutableThis = const_cast<FileSpecificDiagnosticConsumer*>(this);
    mutableThis->computeConsumersOrderedByRange(SM);
  }

  // This std::lower_bound call is doing a binary search for the first range
  // that /might/ contain 'loc'. Specifically, since the ranges are sorted
  // by end location, it's looking for the first range where the end location
  // is greater than or equal to 'loc'.
  const ConsumerAndRange *possiblyContainingRangeIter = std::lower_bound(
      ConsumersOrderedByRange.begin(), ConsumersOrderedByRange.end(), loc,
      [](const ConsumerAndRange &entry, SourceLoc loc) -> bool {
        return entry.endsAfter(loc);
      });

  if (possiblyContainingRangeIter != ConsumersOrderedByRange.end() &&
      possiblyContainingRangeIter->contains(loc)) {
    auto *consumerAndRangeForLocation =
        const_cast<ConsumerAndRange *>(possiblyContainingRangeIter);
    return &(*this)[*consumerAndRangeForLocation];
  }

  return None;
}

void FileSpecificDiagnosticConsumer::handleDiagnostic(
    SourceManager &SM, SourceLoc Loc, DiagnosticKind Kind,
    StringRef FormatString, ArrayRef<DiagnosticArgument> FormatArgs,
    const DiagnosticInfo &Info,
    const SourceLoc bufferIndirectlyCausingDiagnostic) {

  HasAnErrorBeenConsumed |= Kind == DiagnosticKind::Error;

  auto subconsumer =
      findSubconsumer(SM, Loc, Kind, bufferIndirectlyCausingDiagnostic);
  if (subconsumer) {
    subconsumer.getValue()->handleDiagnostic(SM, Loc, Kind, FormatString,
                                             FormatArgs, Info,
                                             bufferIndirectlyCausingDiagnostic);
    return;
  }
  // Last resort: spray it everywhere
  for (auto &subconsumer : Subconsumers)
    subconsumer.handleDiagnostic(SM, Loc, Kind, FormatString, FormatArgs, Info,
                                 bufferIndirectlyCausingDiagnostic);
}

Optional<FileSpecificDiagnosticConsumer::Subconsumer *>
FileSpecificDiagnosticConsumer::findSubconsumer(
    SourceManager &SM, SourceLoc loc, DiagnosticKind Kind,
    SourceLoc bufferIndirectlyCausingDiagnostic) {
  // Ensure that a note goes to the same place as the preceeding non-note.
  switch (Kind) {
  case DiagnosticKind::Error:
  case DiagnosticKind::Warning:
  case DiagnosticKind::Remark: {
    auto subconsumer =
        findSubconsumerForNonNote(SM, loc, bufferIndirectlyCausingDiagnostic);
    SubconsumerForSubsequentNotes = subconsumer;
    return subconsumer;
  }
  case DiagnosticKind::Note:
    return SubconsumerForSubsequentNotes;
  }
}

Optional<FileSpecificDiagnosticConsumer::Subconsumer *>
FileSpecificDiagnosticConsumer::findSubconsumerForNonNote(
    SourceManager &SM, const SourceLoc loc,
    const SourceLoc bufferIndirectlyCausingDiagnostic) {
  if (auto subconsumer = subconsumerForLocation(SM, loc))
    return subconsumer; // A primary file with a .dia file
  // The diagnostic is located in a non-primary.
  // Try to put it in the current primary file's .dia file.
  if (bufferIndirectlyCausingDiagnostic.isValid())
    return subconsumerForLocation(SM, bufferIndirectlyCausingDiagnostic);
  return None;
}

bool FileSpecificDiagnosticConsumer::finishProcessing() {
  tellSubconsumersToInformDriverOfIncompleteBatchModeCompilation();

  // Deliberately don't use std::any_of here because we don't want early-exit
  // behavior.

  bool hadError = false;
  for (auto &subconsumer : Subconsumers)
    hadError |= subconsumer.getConsumer() &&
                subconsumer.getConsumer()->finishProcessing();
  return hadError;
}

void FileSpecificDiagnosticConsumer::
    tellSubconsumersToInformDriverOfIncompleteBatchModeCompilation() {
  if (!HasAnErrorBeenConsumed)
    return;
  for (auto &info : ConsumersOrderedByRange)
    (*this)[info].informDriverOfIncompleteBatchModeCompilation();
}

void NullDiagnosticConsumer::handleDiagnostic(
    SourceManager &SM, SourceLoc Loc, DiagnosticKind Kind,
    StringRef FormatString, ArrayRef<DiagnosticArgument> FormatArgs,
    const DiagnosticInfo &Info, const SourceLoc) {
  LLVM_DEBUG({
    llvm::dbgs() << "NullDiagnosticConsumer received diagnostic: ";
    DiagnosticEngine::formatDiagnosticText(llvm::dbgs(), FormatString,
                                           FormatArgs);
    llvm::dbgs() << "\n";
  });
}

ForwardingDiagnosticConsumer::ForwardingDiagnosticConsumer(DiagnosticEngine &Target)
  : TargetEngine(Target) {}

void ForwardingDiagnosticConsumer::handleDiagnostic(
    SourceManager &SM, SourceLoc Loc, DiagnosticKind Kind,
    StringRef FormatString, ArrayRef<DiagnosticArgument> FormatArgs,
    const DiagnosticInfo &Info,
    const SourceLoc bufferIndirectlyCausingDiagnostic) {
  LLVM_DEBUG({
    llvm::dbgs() << "ForwardingDiagnosticConsumer received diagnostic: ";
    DiagnosticEngine::formatDiagnosticText(llvm::dbgs(), FormatString,
                                           FormatArgs);
    llvm::dbgs() << "\n";
  });
  for (auto *C : TargetEngine.getConsumers()) {
    C->handleDiagnostic(SM, Loc, Kind, FormatString, FormatArgs, Info,
                        bufferIndirectlyCausingDiagnostic);
  }
}
