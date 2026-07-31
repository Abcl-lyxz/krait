#pragma once

#include "session/profile.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace krait::app {

// Imports mRemoteNG's confCons.xml.
//
// This lives in src/app rather than beside the other two importers in
// src/app/session because it needs an XML parser, and src/app/session is
// deliberately Qt-free (see the note at the top of its CMakeLists) so that
// profiles and palette ranking stay testable without a QCoreApplication. Hand-
// rolling XML for a file the user points us at would be the wrong trade: a real
// parser costs one dependency the app layer already has, QXmlStreamReader out
// of Qt6::Core.
//
// PASSWORDS ARE NEVER IMPORTED. mRemoteNG encrypts them with a key derived from
// a password that defaults to a published constant, so "importing" them would
// mean decrypting a file of credentials with a key everybody has and writing
// them into a second store. Krait's vault is DPAPI-backed and per-user
// (rules/net.md); credentials get re-entered once, deliberately.
struct MremotengImport {
    std::vector<session::Profile> profiles;
    // Connections understood but not importable, with the reason — a protocol
    // Krait does not speak, mostly. Named rather than counted, so the summary
    // can say WHICH ones were left behind.
    QStringList skipped;
    // Set when the file could not be used at all: not a mRemoteNG file, a
    // version newer than the layout known here, or encrypted whole.
    QString error;
};

// Parses the TEXT of a confCons.xml. Never touches the filesystem, which is
// what lets the mapping be tested without one.
MremotengImport importFromMremoteng(const QString& xml);

}  // namespace krait::app
