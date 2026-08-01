import QtQuick
import Krait

Window {
    id: root
    width: 960
    height: 540
    visible: false  // shown from main() after the graphics configuration
    title: qsTr("Krait")
    color: "#0d0f17"

    // The tabs. A ListModel of { title, accent } mirroring what each
    // SessionPane reports, because a Repeater delegate cannot be read back as
    // a model and the strip needs something to bind to.
    //
    // rules/ui.md keeps decisions out of views, and this is not one: which
    // profile a tab opens is decided in C++ (TerminalItem::openProfileById),
    // and all this holds is how many panes exist and which is in front.
    ListModel {
        id: tabs
        ListElement { title: ""; accent: ""; broadcasting: false }
    }

    // T74. ONE broadcast for the window, because that is what it broadcasts
    // ACROSS. Owned here for the same reason the tab list is: every pane binds
    // to it, and two copies would each believe a different set of sessions was
    // receiving. main() hands it the settings registry by findChildren, the way
    // it does for the settings page.
    BroadcastModel {
        id: broadcastModel

        // A held destructive line, and anything the fan-out has to report,
        // land on the CURRENT tab's banner — never a dialog (rules/ui.md), and
        // never N banners, which is what connecting this inside each pane
        // would have produced.
        onConfirmRequested: (message, detail) => {
            const pane = root.currentPane()
            if (pane) pane.showBroadcastConfirm(message, detail)
        }
        onReported: (message, detail) => {
            const pane = root.currentPane()
            if (pane) pane.raiseSessionNotice(message, detail)
        }
    }

    property int currentTab: 0

    function currentPane() {
        return paneRepeater.itemAt(root.currentTab)
    }

    // Returns the new tab's pane. The Repeater creates the delegate
    // SYNCHRONOUSLY inside append(), so there is nothing to wait for — and
    // deferring with Qt.callLater was worse than pointless: it opened a window
    // in which a queued Ctrl+Tab or a tab click could move currentTab, and the
    // deferred callback would then act on a different tab than the one it
    // created. That is how a saved session gets opened over a running one.
    function addTab() {
        tabs.append({ title: "", accent: "", broadcasting: false })
        root.currentTab = tabs.count - 1
        const pane = root.currentPane()
        if (pane) {
            pane.focusCurrent()
        }
        return pane
    }

    function closeTab(index) {
        // The last tab closing closes the window, the way every tabbed terminal
        // behaves — and the alternative, an empty frame with a + button, is a
        // state nobody wants to look at.
        if (tabs.count <= 1) {
            root.close()
            return
        }
        tabs.remove(index)
        // Removing a tab BEFORE the current one shifts every survivor down, so
        // clamping alone is not enough: closing tab 0 while viewing tab 1 left
        // currentTab at 1, which is now what used to be tab 2. Silently landing
        // on a different host is exactly what the prod accent exists to stop.
        if (index < root.currentTab) {
            root.currentTab -= 1
        } else {
            root.currentTab = Math.min(root.currentTab, tabs.count - 1)
        }
        const pane = root.currentPane()
        if (pane) {
            pane.focusCurrent()
        }
    }

    function selectTab(index) {
        if (index < 0 || index >= tabs.count) {
            return
        }
        root.currentTab = index
        const pane = root.currentPane()
        if (pane) {
            pane.focusCurrent()
        }
    }

    function stepTab(step) {
        selectTab((root.currentTab + step + tabs.count) % tabs.count)
    }

    // Opens a saved session in a NEW tab. Opening it over whatever the current
    // tab is running is how you lose a build log.
    function openSession(profileId) {
        const pane = root.addTab()
        if (!pane) {
            return
        }
        if (!pane.openSession(profileId)) {
            // The palette listed it, so this means the store changed
            // underneath. Saying so beats a tab that silently holds a local
            // shell instead of the server that was asked for.
            pane.raiseSessionError(qsTr("That session is no longer saved."), profileId)
        }
    }

    // Something that failed outside any session — so far only a drop-down
    // hotkey another program already owns. Invoked by name from main(), because
    // it is discovered after the tree is built and so cannot ride the
    // launchError context property the command line uses.
    function raiseGlobalError(message, detail) {
        const pane = root.currentPane()
        if (pane) {
            pane.raiseSessionError(message, detail)
        }
    }

    // --- keyboard (rules/ui.md: every action has a binding, none is mouse-only)

    Shortcut { sequence: "Ctrl+Shift+P"; onActivated: palette.open() }
    Shortcut { sequence: "Ctrl+Shift+O"; onActivated: palette.open() }
    Shortcut { sequence: "Ctrl+,"; onActivated: settingsPage.open() }
    Shortcut { sequence: "Ctrl+Shift+T"; onActivated: root.addTab() }
    Shortcut { sequence: "Ctrl+Shift+W"; onActivated: root.closeTab(root.currentTab) }
    Shortcut { sequence: "Ctrl+Tab"; onActivated: root.stepTab(1) }
    Shortcut { sequence: "Ctrl+Shift+Tab"; onActivated: root.stepTab(-1) }
    Shortcut { sequence: "Ctrl+Shift+D"; onActivated: root.splitCurrent(false) }
    Shortcut { sequence: "Ctrl+Shift+U"; onActivated: { const p = root.currentPane(); if (p) p.toggleTunnels() } }
    Shortcut { sequence: "Ctrl+Shift+B"; onActivated: { const p = root.currentPane(); if (p) p.toggleFiles() } }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: { const p = root.currentPane(); if (p) p.toggleSnippets() } }
    Shortcut { sequence: "Ctrl+Shift+K"; onActivated: { const p = root.currentPane(); if (p) p.toggleCopyMode() } }
    Shortcut { sequence: "Ctrl+Shift+L"; onActivated: { const p = root.currentPane(); if (p) p.toggleLogging() } }
    Shortcut { sequence: "Ctrl+Shift+E"; onActivated: root.splitCurrent(true) }
    Shortcut { sequence: "Ctrl+Shift+A"; onActivated: { const p = root.currentPane(); if (p) p.toggleBroadcast() } }
    Shortcut { sequence: "Ctrl+Shift+Up"; onActivated: { const p = root.currentPane(); if (p) p.jumpPrompt(-1) } }
    Shortcut { sequence: "Ctrl+Shift+Down"; onActivated: { const p = root.currentPane(); if (p) p.jumpPrompt(1) } }
    Shortcut { sequence: "Ctrl+Alt+Right"; onActivated: root.stepPane(1) }
    Shortcut { sequence: "Ctrl+Alt+Left"; onActivated: root.stepPane(-1) }

    // Ctrl+1..9 jumps to a tab, and Ctrl+9 means LAST tab rather than tab 9 —
    // the convention every browser uses, and the useful one once there are more
    // than nine.
    //
    // Written out rather than generated: a Repeater delegate must be an Item
    // and Shortcut is not one ("Delegate must be of Item type"), and reaching
    // for Instantiator to save eight lines is machinery for its own sake.
    Shortcut { sequence: "Ctrl+1"; onActivated: root.selectTab(0) }
    Shortcut { sequence: "Ctrl+2"; onActivated: root.selectTab(1) }
    Shortcut { sequence: "Ctrl+3"; onActivated: root.selectTab(2) }
    Shortcut { sequence: "Ctrl+4"; onActivated: root.selectTab(3) }
    Shortcut { sequence: "Ctrl+5"; onActivated: root.selectTab(4) }
    Shortcut { sequence: "Ctrl+6"; onActivated: root.selectTab(5) }
    Shortcut { sequence: "Ctrl+7"; onActivated: root.selectTab(6) }
    Shortcut { sequence: "Ctrl+8"; onActivated: root.selectTab(7) }
    Shortcut { sequence: "Ctrl+9"; onActivated: root.selectTab(tabs.count - 1) }

    function splitCurrent(stacked) {
        const pane = root.currentPane()
        if (pane) {
            pane.split(stacked)
        }
    }

    function stepPane(step) {
        const pane = root.currentPane()
        if (pane) {
            pane.focusNextPane(step)
        }
    }

    // Bench runs keep the synthetic spike; normal runs are the terminal.
    SpikeGrid {
        anchors.fill: parent
        visible: benchMode
    }

    Column {
        anchors.fill: parent
        visible: !benchMode

        TabStrip {
            id: strip
            width: parent.width
            tabs: tabs
            currentIndex: root.currentTab
            onSelected: (index) => root.selectTab(index)
            onClosed: (index) => root.closeTab(index)
            onNewTab: root.addTab()
        }

        Item {
            width: parent.width
            height: parent.height - strip.height

            Repeater {
                id: paneRepeater
                model: tabs

                delegate: SessionPane {
                    id: pane
                    required property int index

                    anchors.fill: parent
                    broadcast: broadcastModel
                    // Only the current tab is visible, and only it takes keys.
                    // The others keep running: a build in tab 2 must not stop
                    // because tab 1 is in front.
                    visible: pane.index === root.currentTab
                    enabled: visible

                    // Mirror into the tab model so the strip can bind. Writing
                    // through onTitleChanged rather than a binding because
                    // ListModel rows are not bindable targets.
                    onTitleChanged: tabs.setProperty(pane.index, "title", pane.title)
                    onAccentChanged: tabs.setProperty(pane.index, "accent", pane.accent)
                    // T74. The tab strip's broadcast mark, so a tab that is
                    // receiving says so from behind the one in front.
                    onBroadcastingChanged:
                        tabs.setProperty(pane.index, "broadcasting", pane.broadcasting)
                    Component.onCompleted: {
                        tabs.setProperty(pane.index, "title", pane.title)
                        tabs.setProperty(pane.index, "accent", pane.accent)
                        tabs.setProperty(pane.index, "broadcasting", pane.broadcasting)
                    }

                    // Closing the last pane in a tab closes the tab.
                    onEmptied: root.closeTab(pane.index)
                }
            }
        }
    }

    Palette {
        id: palette
        z: 100

        // The session itself is opened in C++ (TerminalItem::openProfileById);
        // all that happens here is deciding it lands in a new tab.
        onSessionChosen: (profileId) => root.openSession(profileId)

        onActionChosen: (actionId) => {
            // The action registry is the single source of truth for what exists
            // (src/app/session/actions.cpp); this is where the ids become
            // behaviour. An id with no case here is reported rather than
            // silently ignored.
            switch (actionId) {
            case "settings.open": settingsPage.open(); return
            case "session.new": root.addTab(); return
            case "session.close": root.closeTab(root.currentTab); return
            case "session.next": root.stepTab(1); return
            case "session.previous": root.stepTab(-1); return
            case "view.splitRight": root.splitCurrent(false); return
            case "view.splitDown": root.splitCurrent(true); return
            case "view.tunnels": {
                const pane = root.currentPane()
                if (pane) pane.toggleTunnels()
                return
            }
            case "view.files": {
                const pane = root.currentPane()
                if (pane) pane.toggleFiles()
                return
            }
            case "view.snippets": {
                const pane = root.currentPane()
                if (pane) pane.toggleSnippets()
                return
            }
            case "view.previousPrompt": {
                const pane = root.currentPane()
                if (pane) pane.jumpPrompt(-1)
                return
            }
            case "view.nextPrompt": {
                const pane = root.currentPane()
                if (pane) pane.jumpPrompt(1)
                return
            }
            case "view.hexdump": {
                const pane = root.currentPane()
                if (pane && pane.terminal) {
                    pane.terminal.setHexdump(!pane.terminal.hexdumpEnabled())
                }
                return
            }
            case "session.log": {
                const pane = root.currentPane()
                if (pane) pane.toggleLogging()
                return
            }
            case "view.copyMode": {
                const pane = root.currentPane()
                if (pane) pane.toggleCopyMode()
                return
            }
            case "session.broadcast": {
                const pane = root.currentPane()
                if (pane) pane.toggleBroadcast()
                return
            }
            case "view.closePane": {
                const pane = root.currentPane()
                if (pane) pane.closePane()
                return
            }
            case "palette.open": palette.open(); return
            // T62, and T44's PuTTY importer with them: all three were reaching
            // the "not wired up yet" line below, so that one had never been
            // runnable from the UI at all.
            case "sessions.import.putty":
            case "sessions.import.sshconfig":
            case "sessions.import.mremoteng": {
                const summary = palette.runImport(actionId)
                const target = root.currentPane()
                // A banner, never a dialog (rules/ui.md). The summary names
                // what was skipped, which is the part worth reading.
                if (target && summary.length > 0) target.raiseSessionError(summary, "")
                return
            }
            }
            const pane = root.currentPane()
            if (pane) {
                pane.raiseSessionError(qsTr("Not wired up yet: %1").arg(actionId), "")
            }
        }

        onDismissed: {
            const pane = root.currentPane()
            if (pane) pane.focusCurrent()
        }
    }

    Settings {
        id: settingsPage
        z: 90
        onDismissed: {
            const pane = root.currentPane()
            if (pane) pane.focusCurrent()
        }
    }

    // A command line naming a session that is not saved. Reported once the
    // window exists, because at the point main() knows about it there is no tab
    // to put a banner in.
    Component.onCompleted: {
        if (launchError.length > 0) {
            const pane = root.currentPane()
            if (pane) {
                pane.raiseSessionError(launchError, launchErrorDetail)
            }
        }
    }

    // KRAIT_UI_SELFTEST: exercise the tab and split plumbing and print what
    // happened. The Catch2 suite cannot reach QML, so without this the only
    // evidence that opening a tab works is that the file compiled.
    //
    // Deliberately checks COUNTS rather than pixels — that a split really added
    // a pane and closing really removed it — which is the part that silently
    // breaks when a binding is wrong.
    // Invoked from main() on a singleShot when KRAIT_UI_SELFTEST is set — the
    // same mechanism as the screenshot hook, and deliberately NOT a QML Timer:
    // one declared here reports running == true and never triggers in a window
    // that is never composited, which is exactly how this runs unattended.
    function runSelfTest() {
            console.info("selftest: begin")
            if (!root.currentPane()) {
                console.warn("selftest: no pane for tab " + root.currentTab)
                return
            }
            console.info("selftest: tabs=" + tabs.count + " panes=" + root.currentPane().paneCount)
            root.addTab()
            console.info("selftest: after addTab tabs=" + tabs.count + " current=" + root.currentTab)
            root.splitCurrent(false)
            root.splitCurrent(false)
            console.info("selftest: after 2 splits panes=" + root.currentPane().paneCount +
                        " stacked=" + root.currentPane().stacked +
                        " offset1=" + root.currentPane().offsetOf(1).toFixed(3))
            root.currentPane().moveDivider(0, 0.25)
            console.info("selftest: after drag offset1=" + root.currentPane().offsetOf(1).toFixed(3))
            root.currentPane().closePane()
            console.info("selftest: after closePane panes=" + root.currentPane().paneCount)
            root.stepTab(1)
            console.info("selftest: after stepTab current=" + root.currentTab)
            root.closeTab(root.currentTab)
            console.info("selftest: after closeTab tabs=" + tabs.count +
                        " title='" + tabs.get(0).title + "'")

        // Regressions for the two the first version of this self-test missed.
        //
        // A) Closing a tab BEFORE the current one used to leave currentTab
        //    pointing one place too high, silently moving the user to the next
        //    session along. `keep` is the pane that must still be in front
        //    afterwards; comparing the object, not the index, is the point.
        root.addTab()
        root.addTab()
        root.selectTab(1)
        const keep = root.currentPane()
        root.closeTab(0)
        console.info("selftest: closeTab(0) from tab 1 -> current=" + root.currentTab +
                     " sameSession=" + (root.currentPane() === keep))

        // B) Closing a pane that is NOT the last one used to leave the tab's
        //    `terminal` binding on the destroyed pane, so the strip kept the
        //    closed session's title and its safety accent.
        root.splitCurrent(false)
        const pane = root.currentPane()
        pane.currentPane = 0
        pane.closePane()
        console.info("selftest: after closing pane 0 panes=" + pane.paneCount +
                     " terminalAlive=" + (pane.terminal !== null) +
                     " title='" + pane.title + "'")

        // C) T74. The broadcast wiring is QML on both ends — every pane offers
        //    its terminal, the strip renders the model's rows — and none of
        //    that is reachable from the Catch2 suite, which tests the model
        //    against a plain QObject. What this checks is that the two halves
        //    are actually connected: that panes registered themselves, that
        //    arming reaches Active, and that a line lands in a real session.
        const bpane = root.currentPane()
        console.info("selftest: broadcast targets=" + broadcastModel.tabs.length)
        bpane.toggleBroadcast()
        console.info("selftest: broadcast ready state=" + broadcastModel.state)
        // Ticked the way a user does, from the strip. bpane.terminal is NOT
        // used to pick one here: the delegates closed above are deleteLater'd
        // and this function runs in a single turn, so the tab's `terminal`
        // binding can still resolve to one of them. That window does not exist
        // between two keystrokes, which is the only way a person reaches this.
        broadcastModel.toggleAt(0)
        broadcastModel.arm()
        const before = broadcastModel.sentCount
        broadcastModel.send("echo krait-broadcast-selftest")
        console.info("selftest: broadcast armed state=" + broadcastModel.state +
                     " marked=" + broadcastModel.markedCount +
                     " sent=" + (broadcastModel.sentCount > before))
        broadcastModel.stop()
        console.info("selftest: broadcast stopped state=" + broadcastModel.state +
                     " marked=" + broadcastModel.markedCount)

        console.info("selftest: done")
    }
}
