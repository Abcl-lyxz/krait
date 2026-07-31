import QtQuick
import Krait

// One tab (plan T53): a banner strip over one or more terminals.
//
// ponytail: the panes are a FLAT list sharing ONE orientation per tab, not a
// tree. Two or three terminals side by side is what splitting actually gets
// used for, and a real tree costs a recursive component plus split-point
// bookkeeping for a layout nobody has asked for yet. Upgrade path if they do:
// replace `panes` with a tree and this Row/Column with a recursive Loader.
Item {
    id: tab

    // false = side by side (vertical dividers); true = stacked.
    property bool stacked: false
    property int currentPane: 0
    readonly property int paneCount: panes.count
    // panes.count is read for its DEPENDENCY, not its value. itemAt() is an
    // invokable method, so it registers nothing, which left currentPane as the
    // binding's only reactive input — and closing a pane that is not the last
    // one leaves currentPane unchanged. The binding then kept pointing at the
    // destroyed pane, and since `title` and `accent` hang off it, the tab strip
    // went on showing a closed prod session's name and its red accent over a
    // tab that now held a local shell. rules/ui.md calls that accent a core UX
    // invariant; this is what stops it lying.
    readonly property Item terminal: (panes.count, repeater.itemAt(tab.currentPane))
    // What the tab strip shows. Follows the FOCUSED pane, so splitting a tab
    // and connecting the new pane to prod relabels the tab.
    readonly property string title: terminal ? terminal.sessionTitle : ""
    readonly property string accent: terminal ? terminal.sessionAccent : ""

    // Emitted when the last pane is closed — the tab has nothing left in it.
    signal emptied

    // Credential prompts that arrived while the banner was busy with another
    // pane's. One banner and N panes means they have to queue somewhere, and
    // dropping them means a backend waits for an answer nobody can give.
    property var pendingCredentials: []

    function queueCredential(pane, prompt, echo) {
        tab.pendingCredentials.push({ pane: pane, prompt: prompt, echo: echo })
    }

    // Re-raises the oldest held prompt, skipping any whose pane has since been
    // closed. Called from the banner once it is free again.
    function nextCredential() {
        while (tab.pendingCredentials.length > 0) {
            const held = tab.pendingCredentials.shift()
            if (!held.pane) {
                continue
            }
            tab.showCredential(held.pane, held.prompt, held.echo)
            return
        }
    }

    // Puts a credential prompt on the banner. Both the live signal and the
    // queue land here so there is one description of what that banner looks
    // like, rather than two that drift.
    function showCredential(pane, prompt, echo) {
        banner.target = pane
        banner.mode = "credential"
        banner.severity = "warning"
        banner.showAccept = true
        banner.acceptText = qsTr("Connect")
        banner.rejectText = qsTr("Cancel")
        banner.detail = ""
        // The prompt is SERVER-CONTROLLED and Banner is visible only while its
        // message is non-empty, so three empty keyboard-interactive fields
        // would leave an invisible banner over a five-minute wait.
        banner.message = prompt.length > 0 ? prompt
                                           : qsTr("The server is asking for a password.")
        // Only offer to remember an actual stored secret. A keyboard-interactive
        // challenge is often a one-time code, and saving one saves nothing.
        banner.beginInput(echo, !echo)
    }

    function focusCurrent() {
        if (terminal) {
            terminal.forceActiveFocus()
        }
    }

    // Adds a pane beside the current one. The FIRST split fixes the tab's
    // orientation; splitting the other way afterwards is a no-op rather than a
    // silent relayout of everything already open.
    function split(wantStacked) {
        if (panes.count === 1) {
            tab.stacked = wantStacked
        }
        panes.append({ weight: 1.0 })
        tab.currentPane = panes.count - 1
        focusCurrent()
    }

    function closePane() {
        // A banner aimed at the pane being closed goes with it. Otherwise a
        // credential prompt raised by that pane stays on screen, focused, with
        // its password field live — and the next thing typed into it is a
        // password being handed to a backend that is being torn down, or
        // swallowed while the user believes it was sent.
        if (banner.target === repeater.itemAt(tab.currentPane)) {
            banner.dismiss()
        }
        if (panes.count <= 1) {
            tab.emptied()
            return
        }
        panes.remove(tab.currentPane)
        tab.currentPane = Math.min(tab.currentPane, panes.count - 1)
        focusCurrent()
    }

    function focusNextPane(step) {
        if (panes.count === 0) {
            return
        }
        tab.currentPane = (tab.currentPane + step + panes.count) % panes.count
        focusCurrent()
    }

    // Opens `profileId` in the focused pane. False when the store no longer has
    // it, which the caller turns into a banner.
    function openSession(profileId) {
        return terminal ? terminal.openProfileById(profileId) : false
    }

    // A banner for something that did not come from a backend — the command
    // line naming a session that is not saved, an action with no handler.
    // rules/ui.md makes the per-tab banner the only error surface, so callers
    // outside the backend path still need a way in.
    function raiseSessionError(message, detail) {
        banner.target = null
        banner.mode = "error"
        banner.severity = "error"
        banner.showAccept = false
        banner.rejectText = qsTr("Dismiss")
        banner.detail = detail
        banner.message = message
        banner.forceActiveFocus()
    }

    ListModel {
        id: panes
        // Every tab starts with one terminal. `weight` is the pane's share of
        // the long axis; dividers move it.
        ListElement { weight: 1.0 }
    }

    Column {
        anchors.fill: parent

        Banner {
            id: banner
            width: parent.width

            // WHICH terminal asked. With splits the focused pane is not
            // necessarily the one that raised the prompt, and answering a
            // password into the wrong pane would send it to the wrong host.
            property Item target: null
            property string mode: "paste"

            onModeChanged: if (mode !== "credential") banner.endInput()

            function dismiss() {
                banner.endInput()
                banner.message = ""
                banner.mode = "paste"
                banner.target = null
                tab.focusCurrent()
                // Whatever was waiting behind this one gets its turn now.
                tab.nextCredential()
            }

            onAccepted: {
                const to = banner.target
                if (to) {
                    if (banner.mode === "credential") {
                        to.respondCredential(banner.inputText, banner.remember)
                    } else if (banner.mode === "hostkey") {
                        to.respondHostKey(true)
                    } else if (banner.mode === "paste") {
                        to.resolvePaste(true)
                    }
                }
                banner.dismiss()
            }
            onRejected: {
                const to = banner.target
                if (to) {
                    if (banner.mode === "credential") {
                        // An empty answer is how the backend hears "cancelled".
                        to.respondCredential("", false)
                    } else if (banner.mode === "hostkey") {
                        to.respondHostKey(false)
                    } else if (banner.mode === "paste") {
                        to.resolvePaste(false)
                    }
                }
                banner.dismiss()
            }
        }

        // One axis, chosen by the first split. The panes are positioned by
        // weight rather than laid out in a Row, so a divider drag moves one
        // boundary instead of re-flowing everything after it.
        Item {
            id: area
            width: parent.width
            height: parent.height - banner.height

            Repeater {
                id: repeater
                model: panes

                delegate: TerminalView {
                    id: view
                    required property int index
                    required property real weight

                    width: tab.stacked ? area.width : area.width * (weight / tab.totalWeight)
                    height: tab.stacked ? area.height * (weight / tab.totalWeight) : area.height
                    x: tab.stacked ? 0 : tab.offsetOf(index) * area.width
                    y: tab.stacked ? tab.offsetOf(index) * area.height : 0
                    focus: index === tab.currentPane

                    // Clicking an unfocused pane focuses it, the way every
                    // tiling terminal behaves.
                    onActiveFocusChanged: if (activeFocus) tab.currentPane = index

                    onErrorRaised: (message, hint) => {
                        // A changed host key arrives as TWO signals — the
                        // prompt carrying the fingerprint, then the taxonomy
                        // error. Letting the second overwrite the first meant
                        // the danger banner lived less than a frame and the
                        // evidence was never seen (rules/net.md).
                        if (banner.mode === "hostkey" && banner.target === view) {
                            return
                        }
                        banner.target = view
                        banner.mode = "error"
                        banner.severity = "error"
                        banner.showAccept = false
                        banner.rejectText = qsTr("Dismiss")
                        banner.detail = hint
                        banner.message = message
                        banner.forceActiveFocus()
                    }

                    onPasteConfirmRequested: (message, detail) => {
                        banner.target = view
                        banner.mode = "paste"
                        banner.severity = "warning"
                        banner.showAccept = true
                        banner.acceptText = qsTr("Paste anyway")
                        banner.rejectText = qsTr("Cancel")
                        banner.detail = detail
                        banner.message = message
                        banner.forceActiveFocus()
                    }

                    // `askable` false is a changed or unverifiable key: no Trust
                    // button at all, because rules/net.md says that is never a
                    // question and a button that refuses is worse than none.
                    onHostKeyPromptRequested: (message, detail, askable) => {
                        banner.target = view
                        banner.mode = "hostkey"
                        banner.severity = askable ? "warning" : "danger"
                        banner.showAccept = askable
                        banner.acceptText = qsTr("Trust this server")
                        banner.rejectText = askable ? qsTr("Do not connect") : qsTr("Dismiss")
                        banner.detail = detail
                        banner.message = message
                        banner.forceActiveFocus()
                    }

                    onCredentialPromptRequested: (prompt, echo) => {
                        // Two panes connecting at once: the second prompt must
                        // NOT take the banner from the first. Stealing it left
                        // the first pane's backend waiting for an answer that
                        // could never arrive, until it died on a timeout whose
                        // message named the wrong cause. Held instead, and
                        // re-raised when the banner clears.
                        if (banner.mode === "credential" && banner.target !== view) {
                            tab.queueCredential(view, prompt, echo)
                            return
                        }
                        tab.showCredential(view, prompt, echo)
                    }

                    onConnectionNotice: (message) => {
                        if (message.length === 0) {
                            if (banner.mode === "notice" && banner.target === view) {
                                banner.dismiss()
                            }
                            return
                        }
                        banner.target = view
                        banner.mode = "notice"
                        banner.severity = "warning"
                        banner.showAccept = false
                        banner.rejectText = qsTr("Dismiss")
                        banner.detail = ""
                        banner.message = message
                    }
                }
            }

            // The dividers, one between each adjacent pair.
            Repeater {
                model: Math.max(0, panes.count - 1)

                delegate: Rectangle {
                    id: divider
                    required property int index

                    readonly property real edge: tab.offsetOf(divider.index + 1)
                    // TODO(theme): a token once the theme system exists.
                    color: drag.pressed ? "#4c5470" : "#2c3242"
                    width: tab.stacked ? area.width : 4
                    height: tab.stacked ? 4 : area.height
                    x: tab.stacked ? 0 : divider.edge * area.width - 2
                    y: tab.stacked ? divider.edge * area.height - 2 : 0

                    MouseArea {
                        id: drag
                        anchors.fill: parent
                        anchors.margins: -3  // easier to grab than 4 px
                        cursorShape: tab.stacked ? Qt.SplitVCursor : Qt.SplitHCursor
                        onPositionChanged: (mouse) => {
                            if (!pressed) {
                                return
                            }
                            const point = divider.mapToItem(area, mouse.x, mouse.y)
                            const along = tab.stacked ? point.y / area.height
                                                      : point.x / area.width
                            tab.moveDivider(divider.index, along)
                        }
                    }
                }
            }
        }
    }

    // --- weight arithmetic, kept here so both repeaters agree ---

    readonly property real totalWeight: {
        let sum = 0
        for (let i = 0; i < panes.count; ++i) {
            sum += panes.get(i).weight
        }
        return sum > 0 ? sum : 1
    }

    // Fraction of the axis before pane `index`.
    function offsetOf(index) {
        let before = 0
        for (let i = 0; i < index && i < panes.count; ++i) {
            before += panes.get(i).weight
        }
        return before / tab.totalWeight
    }

    // Drags the boundary after pane `index` to `fraction` of the axis, moving
    // weight between that pane and the next one only. Everything else stays
    // put, which is what makes dragging one divider feel local.
    function moveDivider(index, fraction) {
        if (index < 0 || index + 1 >= panes.count) {
            return
        }
        const before = tab.offsetOf(index)
        const after = tab.offsetOf(index + 2 <= panes.count ? index + 2 : panes.count)
        // A pane is never dragged to nothing: below this it cannot be grabbed
        // again, and a zero-width terminal reflows its grid to one column.
        const minimum = 0.05
        // The pair has to be wide enough to hold both minimums, or the clamp
        // below inverts: Math.max wins over Math.min, `share` comes out above
        // 1, and the second pane is written a NEGATIVE weight. totalWeight then
        // shrinks, every offset above it exceeds 1, and the tab lays out
        // off-screen with no way back. Reachable at around seven panes, since
        // each split dilutes the existing fractions while their weights stay.
        if (after - before <= 2 * minimum) {
            return
        }
        const clamped = Math.max(before + minimum, Math.min(after - minimum, fraction))
        const pair = after - before
        const share = pair > 0 ? (clamped - before) / pair : 0.5
        const sum = panes.get(index).weight + panes.get(index + 1).weight
        panes.setProperty(index, "weight", sum * share)
        panes.setProperty(index + 1, "weight", sum * (1 - share))
    }
}
