import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: win
    width: 1000
    height: 760
    minimumWidth: 560
    minimumHeight: 420
    visible: true
    title: backend.open ? backend.title + " — Omaread" : "Omaread"
    color: theme.background
    // The interface follows the desktop's text size: 14 px at its default 12.
    font.pixelSize: Math.round(theme.textSize * 7 / 6)

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property color muted: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.6)
    readonly property int uiSize: font.pixelSize
    // The reading text, as a multiple of the desktop's text size.
    readonly property int readingSize: Math.round(theme.textSize * backend.textScale)
    // The reading settings, as the text view uses them.
    readonly property real lineHeight: backend.lineSpacing === "compact" ? 1.2 : backend.lineSpacing === "relaxed" ? 1.65 : 1.4
    readonly property int columnChars: backend.columnWidth === "narrow" ? 28 : backend.columnWidth === "wide" ? 42 : 34

    // Single-key shortcuts rest while a popup or the search field has the keyboard.
    readonly property bool busy: help.opened || contents.opened || goDialog.opened || passwordDialog.opened
                                 // From the moment it appears, not once its opening animation ends,
                                 // or a key pressed right after S turns the page behind it.
                                 || settings.visible || note.visible
                                 || findField.activeFocus
    readonly property bool reading: backend.open && !busy && settled
    readonly property bool browsing: !backend.open && !busy && settled

    // Hyprland sizes a new window once it's on screen, so the first frames
    // are laid out at the size asked for and then again at the real one. The
    // contents stay hidden until that size is in, or until it's clear none is
    // coming, so they first appear where they'll stay.
    property bool settled: false
    property bool drawn: false
    onFrameSwapped: {
        if (!drawn) { drawn = true; settleTimer.start() }
        // A turn waits two frames, so the picture of the page it leaves is
        // taken before the page changes.
        if (pendingTurn && ++pendingTurn.frames >= 2)
            startTurn()
        else if (pendingTurn)
            requestUpdate()
    }
    onWidthChanged: if (drawn) settled = true
    onHeightChanged: if (drawn) settled = true
    Timer { id: settleTimer; interval: 150; onTriggered: win.settled = true }

    // How pages are drawn: fitted whole into the window, or across its width,
    // and then scaled by zoom.
    property bool fitWidth: false
    property real zoom: 1
    // Set while the view is moved to the saved place, so that move isn't
    // taken for the reader scrolling.
    property bool restoring: false
    property bool finding: false
    property string toast: ""

    readonly property int step: backend.paged && backend.spread ? 2 : 1
    readonly property var shownPages: !backend.paged ? []
        : backend.spread && backend.section + 1 < backend.sectionCount ? [backend.section, backend.section + 1]
        : [backend.section]
    readonly property string tint: backend.tintPages
        ? "/" + String(theme.background).slice(1, 7) + "/" + String(theme.foreground).slice(1, 7) : ""

    // Reading the language makes every binding that formats a number
    // re-render in the new language's digits on a switch.
    function number(n) {
        void translator.language
        return Number(n).toLocaleString(Qt.locale(), 'f', 0)
    }
    function percent(p) { return qsTr("%1%").arg(number(Math.round(p * 100))) }
    function aspect(page) { return backend.aspects[page] || 0.7071 }

    // One screen on; at the end of a section, the start of the next. A flowing
    // book's next screen starts with the line the last one cut off, so every
    // line is read once and whole.
    // When the screen on view began to be read, to learn the reader's pace.
    // Leaving the window, or moving anywhere but on, starts the count again.
    property double screenStarted: 0
    onActiveChanged: screenStarted = active ? Date.now() : 0
    function noteScreenRead() {
        if (!backend.paged && screenStarted > 0 && active && view.contentHeight > 0)
            backend.noteReading((Date.now() - screenStarted) / 1000, Math.min(1, view.height / view.contentHeight))
        screenStarted = Date.now()
    }
    function forward() {
        letLinkGo()
        noteScreenRead()
        if (view.contentY + view.height >= view.contentHeight - 2) {
            if (backend.section + step < backend.sectionCount)
                jump(backend.section + step, 0)
        } else {
            const cut = backend.paged ? -1 : view.height - lineCover.bottom
            const to = cut > lineCover.pad ? view.contentY + cut - lineCover.pad : view.contentY + screenStep()
            turn(1, () => view.contentY = Math.min(to, view.contentHeight - view.height))
        }
    }
    function backward() {
        letLinkGo()
        screenStarted = Date.now()
        if (view.contentY <= 1) {
            if (backend.section > 0)
                jump(Math.max(0, backend.section - step), 1)
        } else {
            // The screen that ends where this one's first whole line begins.
            const first = backend.paged ? -1 : view.contentY + lineCover.top
            const to = first > view.contentY ? first + lineCover.pad - view.height : view.contentY - screenStep()
            turn(-1, () => view.contentY = Math.max(0, Math.min(to, view.contentY - 1)))
        }
    }
    function screenStep() { return Math.max(40, view.height - 3 * readingSize) }
    function scroll(dy) {
        view.contentY = Math.max(0, Math.min(view.contentY + dy, view.contentHeight - view.height))
    }
    // What the text is laid out in, so the backend counts the book's pages
    // the way they are shown, again whenever any of it changes.
    readonly property var pageLayout: [prose.width, view.height, prose.font.family, readingSize, lineHeight, backend.justify]
    onPageLayoutChanged: backend.setPageLayout(prose.width, view.height, prose.font.family, readingSize, lineHeight, backend.justify)

    // What the footer's right side shows. A PDF or comic has no words to time,
    // so it shows pages or the percent read.
    readonly property var progressModes: backend.paged ? ["page", "percent", "none"]
                                                       : ["chapter", "book", "page", "percent", "none"]
    readonly property string progressMode: progressModes.indexOf(backend.progressShows) >= 0 ? backend.progressShows
                                         : backend.paged ? "page" : "chapter"
    function progressName(mode) {
        return mode === "chapter" ? qsTr("Time left in chapter") : mode === "book" ? qsTr("Time left in book")
             : mode === "page" ? qsTr("Page number") : mode === "percent" ? qsTr("Percent read") : qsTr("Progress hidden")
    }
    function nextProgress() {
        const next = progressModes[(progressModes.indexOf(progressMode) + 1) % progressModes.length]
        backend.progressShows = next
        say(progressName(next))
    }
    function timeLeft(minutes, inBook) {
        if (minutes <= 1)
            return inBook ? qsTr("Less than a minute left in book") : qsTr("Less than a minute left in chapter")
        if (inBook && minutes >= 60)
            return qsTr("%1 h %2 min left in book").arg(number(Math.floor(minutes / 60))).arg(number(minutes % 60))
        return inBook ? qsTr("%Ln min left in book", "", minutes) : qsTr("%Ln min left in chapter", "", minutes)
    }
    function whereText() {
        switch (progressMode) {
        case "chapter": return backend.minutesLeftInChapter < 0 ? percent(backend.progress) : timeLeft(backend.minutesLeftInChapter, false)
        case "book": return backend.minutesLeftInBook < 0 ? percent(backend.progress) : timeLeft(backend.minutesLeftInBook, true)
        case "percent": return percent(backend.progress)
        case "none": return ""
        }
        if (!backend.paged)
            return backend.pageCount > 0
                ? qsTr("Page %1 of %2").arg(number(backend.pageAt(backend.section, view.contentY))).arg(number(backend.pageCount))
                : qsTr("Counting pages…")
        if (shownPages.length > 1)
            return qsTr("Pages %1–%2 of %3").arg(number(backend.section + 1)).arg(number(backend.section + 2)).arg(number(backend.sectionCount))
        return qsTr("Page %1 of %2").arg(number(backend.section + 1)).arg(number(backend.sectionCount))
    }

    // A note opens in place; any other link in the book is followed, and a
    // link out of it opens in the browser.
    function openLink(link) {
        const html = backend.note(link)
        if (html !== "")
            note.show(html, link)
        else if (backend.followLink(link))
            hintBack()
        else
            Qt.openUrlExternally(link)
    }
    // Back and forward through the places jumps left. A jump says once or
    // twice that Backspace returns, until the reader knows.
    property int backHints: 0
    function hintBack() {
        if (backHints < 2) {
            ++backHints
            say(qsTr("Backspace to go back"))
        }
    }
    function markJump() {
        backend.markPlace()
        hintBack()
    }
    function goBack() { if (backend.goBack()) Qt.callLater(restore) }
    function goForward() { if (backend.goForward()) Qt.callLater(restore) }
    function jump(section, position) {
        if (section === backend.section) {
            backend.goTo(section, position)
            Qt.callLater(restore)
            return
        }
        turn(section > backend.section ? 1 : -1, () => {
            backend.goTo(section, position)
            Qt.callLater(restore)
        })
    }

    // Page turns: a picture of the page being left is taken, the page
    // changes underneath it, and the two move as the chosen transition says.
    // turnKind is set only while a turn plays; turnProgress runs from 0 to 1.
    property string turnKind: ""
    property int turnDirection: 1
    property real turnProgress: 0
    property var pendingTurn: null
    readonly property bool turning: turnKind !== "" || pendingTurn !== null

    function transitionName(kind) {
        return kind === "slide" ? qsTr("Slide") : kind === "flip" ? qsTr("Flip") : kind === "cover" ? qsTr("Cover")
             : kind === "fade" ? qsTr("Fade") : qsTr("Off")
    }
    function nextTransition() {
        const kinds = backend.transitions
        backend.transition = kinds[(kinds.indexOf(backend.transition) + 1) % kinds.length]
        say(qsTr("Page turn: %1").arg(transitionName(backend.transition)))
    }
    function turn(direction, action) {
        // A turn asked for while one plays ends that one at once.
        if (turnAnimation.running)
            turnAnimation.complete()
        endTurn()
        if (pendingTurn) {
            const waiting = pendingTurn
            pendingTurn = null
            waiting.action()
            action()
            return
        }
        if (backend.transition === "none" || !settled) {
            action()
            return
        }
        pendingTurn = { direction: direction, action: action, frames: 0 }
        snapshot.scheduleUpdate()
        turnTimeout.restart()
    }
    function startTurn() {
        const waiting = pendingTurn
        pendingTurn = null
        turnTimeout.stop()
        turnDirection = waiting.direction
        turnProgress = 0
        turnKind = backend.transition
        waiting.action()
        turnAnimation.restart()
    }
    function endTurn() {
        turnKind = ""
        turnProgress = 0
    }

    NumberAnimation {
        id: turnAnimation
        target: win
        property: "turnProgress"
        from: 0
        to: 1
        duration: win.turnKind === "flip" ? 420 : win.turnKind === "fade" ? 240 : win.turnKind === "cover" ? 300 : 260
        easing.type: win.turnKind === "flip" ? Easing.InOutSine : win.turnKind === "fade" ? Easing.InOutQuad : Easing.OutCubic
        onFinished: win.endTurn()
    }
    // If no frame comes, as when the window is hidden, the page still turns.
    Timer {
        id: turnTimeout
        interval: 150
        onTriggered: if (win.pendingTurn) { const waiting = win.pendingTurn; win.pendingTurn = null; waiting.action() }
    }
    function restore() {
        restoring = true
        view.contentY = backend.position * Math.max(0, view.contentHeight - view.height)
        view.contentX = Math.max(0, (view.contentWidth - view.width) / 2)
        restoring = false
    }
    // Brings a place in the text into view, a little below the top.
    function reveal(y) {
        view.contentY = Math.max(0, Math.min(y - 24, view.contentHeight - view.height))
    }
    function showTarget() {
        if (backend.target >= 0 && !backend.paged)
            reveal(prose.y + prose.positionToRectangle(backend.target).y)
    }
    // The current search match: selected in the text, or scrolled to on a page.
    function showHit() {
        if (backend.hitIndex < 0)
            return
        if (backend.paged) {
            const hit = backend.pageHits.find(h => h.current)
            const at = hit ? shownPages.indexOf(hit.page) : -1
            const sheet = at >= 0 ? sheets.itemAt(at) : null
            if (sheet) {
                const y = spreadRow.y + hit.y * sheet.height
                if (y < view.contentY || y > view.contentY + view.height - 40)
                    reveal(y - view.height / 3)
            }
            return
        }
        const at = backend.hitOffset
        if (at < 0)
            return
        prose.select(at, at + backend.query.length)
        reveal(prose.y + prose.positionToRectangle(at).y - view.height / 3)
    }
    function resize(by) {
        if (backend.paged)
            zoom = by === 0 ? 1 : Math.max(0.5, Math.min(4, zoom * (by > 0 ? 1.2 : 1 / 1.2)))
        else
            backend.textScale = by === 0 ? 1.5 : Math.round((backend.textScale + 0.1 * by) * 10) / 10
    }
    function copy() {
        if (prose.selectedText !== "") {
            backend.copyText(prose.selectedText)
            say(qsTr("Copied"))
            return
        }
        for (let i = 0; i < sheets.count; ++i) {
            const sheet = sheets.itemAt(i)
            if (sheet && sheet.hasSelection) {
                const area = sheet.selected
                say(backend.copyPageText(sheet.page, area.x, area.y, area.width, area.height)
                    ? qsTr("Copied") : qsTr("There are no words there to copy"))
                return
            }
        }
    }
    // The keyboard's way to a link: Tab and Shift+Tab step through the links
    // on screen, Enter opens the one chosen, and Escape lets it go.
    property var chosenLink: null
    function linksOnScreen() {
        const top = view.contentY - prose.y, bottom = top + view.height - lineCover.bottom
        return backend.linksIn(prose.textDocument).filter(link => {
            const y = prose.positionToRectangle(link.start).y
            return y >= top && y < bottom
        })
    }
    function chooseLink(step) {
        const links = linksOnScreen()
        if (links.length === 0) {
            chosenLink = null
            say(qsTr("No links on this page"))
            return
        }
        const at = chosenLink ? links.findIndex(link => link.start === chosenLink.start) : -1
        chosenLink = links[at < 0 ? (step > 0 ? 0 : links.length - 1) : (at + step + links.length) % links.length]
        prose.select(chosenLink.start, chosenLink.end)
    }
    function letLinkGo() {
        if (chosenLink)
            prose.deselect()
        chosenLink = null
    }
    function openChosenLink() {
        const href = chosenLink.href
        letLinkGo()
        openLink(href)
    }
    function clearSelections() {
        chosenLink = null
        prose.deselect()
        for (let i = 0; i < sheets.count; ++i)
            if (sheets.itemAt(i))
                sheets.itemAt(i).clearSelection()
    }
    function say(text) {
        toast = text
        toastTimer.restart()
    }
    function startFinding() {
        finding = true
        findField.text = backend.query
        findField.selectAll()
        findField.forceActiveFocus()
    }
    function stopFinding() {
        finding = false
        backend.clearSearch()
        prose.deselect()
        view.forceActiveFocus()
    }
    function leave() {
        if (chosenLink)
            letLinkGo()
        else if (backend.error !== "")
            backend.dismissError()
        else if (finding)
            stopFinding()
        else if (win.visibility === Window.FullScreen)
            win.showNormal()
        else if (backend.open)
            backend.close()
    }

    Timer { id: toastTimer; interval: 1600; onTriggered: win.toast = "" }

    Connections {
        target: backend
        // Until the new text is laid out and the view moved to its place, the
        // view still scrolled for the old one isn't the reader moving: taken
        // for it, a jump to a chapter's start landed as far down as the place left.
        function onBookChanged() { win.restoring = true; win.zoom = 1; win.finding = false; win.screenStarted = Date.now(); Qt.callLater(win.restore) }
        function onSectionChanged() { win.restoring = true; win.clearSelections(); Qt.callLater(win.restore) }
        // New text, for a new section or a new text size: its layout moves the
        // view, which mustn't move the reader's place.
        function onTextAboutToChange() { win.restoring = true; Qt.callLater(win.restore) }
        function onTargetChanged() { Qt.callLater(win.showTarget) }
        function onHitChanged() { Qt.callLater(win.showHit) }
        function onPasswordChanged() { if (backend.passwordFor !== "") passwordDialog.open() }
    }

    Shortcut { sequence: "?"; enabled: !win.busy || help.opened; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequence: "Q"; enabled: !win.busy; onActivated: Qt.quit() }
    Shortcut { sequence: "O"; enabled: !win.busy; onActivated: backend.pickFile() }
    Shortcut { sequence: "F"; enabled: !win.busy; onActivated: win.visibility === Window.FullScreen ? win.showNormal() : win.showFullScreen() }
    Shortcut { sequence: "Escape"; enabled: !win.busy; onActivated: win.leave() }
    Shortcut { sequences: [StandardKey.Copy]; enabled: backend.open; onActivated: win.copy() }

    Shortcut { sequences: ["Space", "PgDown", "Right"]; enabled: win.reading; onActivated: win.forward() }
    Shortcut { sequences: ["Shift+Space", "PgUp", "Left"]; enabled: win.reading; onActivated: win.backward() }
    Shortcut { sequences: ["Down", "J"]; enabled: win.reading; onActivated: win.scroll(60) }
    Shortcut { sequences: ["Up", "K"]; enabled: win.reading; onActivated: win.scroll(-60) }
    Shortcut { sequence: "]"; enabled: win.reading; onActivated: win.jump(backend.section + win.step, 0) }
    Shortcut { sequence: "["; enabled: win.reading; onActivated: win.jump(Math.max(0, backend.section - win.step), 0) }
    Shortcut { sequence: "Home"; enabled: win.reading; onActivated: { win.markJump(); win.jump(0, 0) } }
    Shortcut { sequence: "End"; enabled: win.reading; onActivated: { win.markJump(); win.jump(backend.sectionCount - 1, 0) } }
    Shortcut { sequence: "Tab"; enabled: win.reading && !backend.paged; onActivated: win.chooseLink(1) }
    Shortcut { sequence: "Backtab"; enabled: win.reading && !backend.paged; onActivated: win.chooseLink(-1) }
    Shortcut { sequences: ["Return", "Enter"]; enabled: win.reading && win.chosenLink !== null; onActivated: win.openChosenLink() }
    Shortcut { sequences: ["Backspace", "Alt+Left"]; enabled: win.reading && backend.canGoBack; onActivated: win.goBack() }
    Shortcut { sequences: ["Shift+Backspace", "Alt+Right"]; enabled: win.reading && backend.canGoForward; onActivated: win.goForward() }
    Shortcut { sequence: "T"; enabled: backend.open && (!win.busy || contents.opened); onActivated: contents.opened ? contents.close() : contents.open() }
    Shortcut { sequence: "G"; enabled: win.reading; onActivated: goDialog.open() }
    Shortcut { sequences: ["/", StandardKey.Find]; enabled: backend.open && settled && !help.opened && !contents.opened && !goDialog.opened; onActivated: win.startFinding() }
    Shortcut { sequences: ["N", StandardKey.FindNext]; enabled: win.reading && backend.hitCount > 0; onActivated: backend.nextHit() }
    Shortcut { sequences: ["Shift+N", StandardKey.FindPrevious]; enabled: win.reading && backend.hitCount > 0; onActivated: backend.previousHit() }
    Shortcut { sequences: ["+", "="]; enabled: win.reading; onActivated: win.resize(1) }
    Shortcut { sequence: "-"; enabled: win.reading; onActivated: win.resize(-1) }
    Shortcut { sequence: "0"; enabled: win.reading; onActivated: win.resize(0) }
    Shortcut { sequence: "W"; enabled: win.reading && backend.paged; onActivated: { win.fitWidth = !win.fitWidth; win.zoom = 1; Qt.callLater(win.restore) } }
    Shortcut { sequence: "D"; enabled: win.reading && backend.paged; onActivated: backend.spread = !backend.spread }
    Shortcut { sequence: "I"; enabled: win.reading && backend.paged; onActivated: backend.tintPages = !backend.tintPages }
    Shortcut {
        sequence: "S"
        enabled: backend.open && !backend.paged && (!win.busy || settings.visible)
        onActivated: settings.visible ? settings.close() : settings.open()
    }
    Shortcut { sequence: "P"; enabled: win.reading; onActivated: win.nextProgress() }
    Shortcut { sequence: "A"; enabled: win.reading; onActivated: win.nextTransition() }

    Shortcut { sequence: "Down"; enabled: win.browsing; onActivated: shelf.incrementCurrentIndex() }
    Shortcut { sequence: "Up"; enabled: win.browsing; onActivated: shelf.decrementCurrentIndex() }
    Shortcut { sequence: "Return"; enabled: win.browsing && shelf.count > 0; onActivated: backend.openRecent(shelf.currentIndex) }
    Shortcut { sequence: "Delete"; enabled: win.browsing && shelf.count > 0; onActivated: backend.forget(shelf.currentIndex) }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true
        enabled: win.settled
        opacity: win.settled ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 100 } }

        StackLayout {
            anchors.fill: parent
            currentIndex: backend.open ? 1 : 0

            // The library: what was read before, most recent first.
            Item {
                objectName: "library"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.topMargin: 48
                    anchors.bottomMargin: 24
                    anchors.leftMargin: Math.max(24, (parent.width - 720) / 2)
                    anchors.rightMargin: anchors.leftMargin
                    spacing: 16
                    visible: shelf.count > 0

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("Library")
                            font.pixelSize: win.uiSize * 2
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: qsTr("O to open · ? for keys")
                            color: win.muted
                        }
                    }

                    ListView {
                        id: shelf
                        objectName: "shelf"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 2
                        model: backend.library
                        currentIndex: 0
                        ScrollBar.vertical: ScrollBar {}

                        delegate: Rectangle {
                            id: row
                            required property int index
                            required property var modelData
                            width: ListView.view.width
                            height: Math.round(win.uiSize * 4.6)
                            radius: Math.min(4, theme.radius)
                            color: ListView.isCurrentItem ? theme.colors.selection : "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 16
                                anchors.rightMargin: 16
                                spacing: 16

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    // Explicit alignments, so mirroring moves them in a
                                    // right-to-left language whatever the title's script.
                                    Label {
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignLeft
                                        text: row.modelData.title
                                        font.pixelSize: Math.round(win.uiSize * 8 / 7)
                                        font.bold: true
                                        elide: Text.ElideRight
                                        opacity: row.modelData.exists ? 1 : 0.5
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignLeft
                                        text: row.modelData.exists
                                              ? (row.modelData.author ? row.modelData.author + " · " : "") + row.modelData.fileName
                                              : qsTr("Missing: %1").arg(row.modelData.fileName)
                                        color: win.muted
                                        elide: Text.ElideRight
                                    }
                                }
                                Label {
                                    text: win.percent(row.modelData.progress)
                                    color: win.muted
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: shelf.currentIndex = row.index
                                onDoubleClicked: backend.openRecent(row.index)
                            }
                        }
                    }
                }

                // Nothing read yet: say how to start, never a blank page.
                ColumnLayout {
                    objectName: "empty"
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 480)
                    spacing: 16
                    visible: shelf.count === 0

                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Nothing to read yet")
                        font.pixelSize: win.uiSize * 2
                        font.bold: true
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("Open a PDF, EPUB, comic, Markdown or text file, or drop one on the window.")
                        color: win.muted
                        wrapMode: Text.Wrap
                    }
                    Button {
                        id: openButton
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Open…")
                        highlighted: true
                        Material.roundedScale: theme.radius
                        focusPolicy: Qt.NoFocus
                        onClicked: backend.pickFile()
                        // A highlighted button colors its caption by Material's
                        // own rule, which can't read on every accent.
                        contentItem: Label {
                            text: openButton.text
                            font: openButton.font
                            color: theme.accentForeground
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }

            // The reader: one section at a time, pages drawn or text laid out.
            Item {
                objectName: "reader"

                // The page in view, on the window's color, so a turn can move
                // it as a whole.
                // Turning pages stay inside the reading area.
                Item {
                    id: turnArea
                    anchors.fill: parent
                    anchors.bottomMargin: statusBar.height + (findBar.visible ? findBar.height : 0)
                    clip: win.turnKind !== ""

                    Item {
                        id: stage
                        objectName: "stage"
                        readonly property bool front: (win.turnKind === "cover" && win.turnDirection > 0)
                                                      || (win.turnKind === "flip" && win.turnDirection < 0)
                        anchors.fill: parent
                        z: front ? 1 : 0
                        // A fade goes out to the window's color and back in.
                        opacity: win.turnKind === "fade" ? Math.max(0, 2 * win.turnProgress - 1) : 1
                        transform: [
                            // Turning back in a book, the page before swings in from the spine.
                            Rotation {
                                origin.x: 0
                                origin.y: stage.height / 2
                                distanceToPlane: 3 * stage.width
                                axis { x: 0; y: 1; z: 0 }
                                angle: win.turnKind === "flip" && win.turnDirection < 0 ? -90 * (1 - win.turnProgress) : 0
                            },
                            Translate {
                                x: win.turnKind === "slide" ? win.turnDirection * stage.width * (1 - win.turnProgress)
                                 : win.turnKind === "cover" && win.turnDirection > 0 ? stage.width * (1 - win.turnProgress) : 0
                            }
                        ]

                        Rectangle {
                            anchors.fill: parent
                            color: theme.background
                        }

                        // A flowing book shows whole lines only: a line the window's
                        // top or bottom edge cuts through is hidden behind the
                        // page's color, and the next page turn starts with it. A
                        // picture taller than that is never hidden.
                        QtObject {
                            id: lineCover
                            readonly property int pad: 24
                            function cutAt(edge) {
                                if (backend.paged || prose.length === 0)
                                    return null
                                const y = edge - prose.y
                                if (y < 0 || y > prose.height)
                                    return null
                                const line = prose.positionToRectangle(prose.positionAt(0, y))
                                return line.y < y && line.y + line.height > y ? line : null
                            }
                            // How much of the top and bottom of the view to cover.
                            readonly property real top: {
                                void (view.contentY + view.height + prose.height + prose.width)
                                const line = cutAt(view.contentY)
                                const cover = line ? prose.y + line.y + line.height - view.contentY : 0
                                return cover < view.height * 0.3 ? cover : 0
                            }
                            readonly property real bottom: {
                                void (view.contentY + view.height + prose.height + prose.width)
                                const edge = view.contentY + view.height
                                const line = cutAt(edge)
                                const cover = line ? edge - prose.y - line.y : 0
                                return cover < view.height * 0.3 ? cover : 0
                            }
                        }

                        Flickable {
                            id: view
                            objectName: "view"
                            anchors.fill: parent
                            clip: true
                            // The mouse selects words; the wheel, the touchpad and the
                            // keys move through the book.
                            acceptedButtons: Qt.NoButton
                            boundsBehavior: Flickable.StopAtBounds
                            contentWidth: backend.paged ? Math.max(width, spreadRow.width + 2 * spreadRow.margin) : width
                            contentHeight: backend.paged ? spreadRow.height + 2 * spreadRow.margin : prose.height + 2 * prose.margin
                            ScrollBar.vertical: ScrollBar {}
                            ScrollBar.horizontal: ScrollBar {}

                            onContentYChanged: if (!win.restoring && contentHeight > height)
                                backend.position = contentY / (contentHeight - height)
                            onContentHeightChanged: Qt.callLater(win.restore)
                            onHeightChanged: Qt.callLater(win.restore)

                            // A paged book's page, or two side by side, fitted to the
                            // window and then zoomed. Books keep their own reading
                            // direction, whatever the interface's.
                            Row {
                                id: spreadRow
                                objectName: "spread"
                                readonly property int margin: 16
                                readonly property real aspects: win.shownPages.reduce((sum, page) => sum + win.aspect(page), 0) || 1
                                readonly property real available: view.width - 2 * margin - spacing * Math.max(0, win.shownPages.length - 1)
                                readonly property real fitted: win.fitWidth ? available / aspects
                                                                            : Math.min(view.height - 2 * margin, available / aspects)
                                readonly property real pageHeight: Math.max(48, fitted * win.zoom)
                                LayoutMirroring.enabled: false
                                visible: backend.paged
                                x: Math.max(margin, (view.width - width) / 2)
                                y: margin
                                spacing: 8

                                Repeater {
                                    id: sheets
                                    model: win.shownPages
                                    delegate: BookPage {
                                        id: pageSheet
                                        required property int modelData
                                        page: modelData
                                        tint: win.tint
                                        highlight: theme.accent
                                        width: Math.round(spreadRow.pageHeight * win.aspect(modelData))
                                        height: Math.round(spreadRow.pageHeight)
                                        onFollow: (target, url) => { if (target >= 0) { win.markJump(); win.jump(target, 0) } else Qt.openUrlExternally(url) }
                                        onSelectionStarted: {
                                            prose.deselect()
                                            for (let i = 0; i < sheets.count; ++i)
                                                if (sheets.itemAt(i) && sheets.itemAt(i) !== pageSheet)
                                                    sheets.itemAt(i).clearSelection()
                                        }
                                    }
                                }
                            }

                            // A flowing book's chapter, in the theme's colors and the
                            // reading text size. Read-only, but its words can be selected.
                            TextEdit {
                                id: prose
                                objectName: "prose"
                                readonly property int margin: 40
                                visible: !backend.paged
                                x: (view.width - width) / 2
                                y: margin
                                width: Math.max(200, Math.min(view.width - 2 * 32, win.readingSize * win.columnChars))
                                text: backend.text
                                textFormat: backend.textFormat
                                baseUrl: backend.baseUrl
                                readOnly: true
                                selectByMouse: true
                                persistentSelection: true
                                // Never the keyboard's: the keys turn pages.
                                activeFocusOnPress: false
                                wrapMode: TextEdit.Wrap
                                font.pixelSize: win.readingSize
                                // Noto Serif comes with Omarchy; elsewhere fontconfig finds the
                                // nearest. The sans is the desktop's own.
                                font.family: backend.typeface === "serif" ? "Noto Serif" : win.font.family
                                color: theme.foreground
                                selectionColor: theme.accent
                                selectedTextColor: theme.accentForeground
                                readonly property real paragraphGap: Math.round(win.readingSize * 0.75)
                                function typeset() { backend.typeset(textDocument, win.lineHeight, paragraphGap, backend.justify) }
                                onTextChanged: {
                                    backend.loadImages(textDocument)
                                    typeset()
                                }
                                onParagraphGapChanged: typeset()
                                Connections {
                                    target: backend
                                    function onSettingsChanged() { prose.typeset() }
                                }
                                onLinkActivated: link => win.openLink(link)

                                HoverHandler {
                                    cursorShape: prose.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor
                                }
                            }
                        }

                        // The lines the view's edges cut through, hidden. The
                        // scroll bar stays above them.
                        Rectangle {
                            objectName: "topCover"
                            anchors { left: parent.left; right: parent.right; top: parent.top; rightMargin: 16 }
                            height: lineCover.top
                            visible: height > 0
                            color: theme.background
                        }
                        Rectangle {
                            objectName: "bottomCover"
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; rightMargin: 16 }
                            height: lineCover.bottom
                            visible: height > 0
                            color: theme.background
                        }

                        // Shadow on a page swinging in.
                        Rectangle {
                            anchors.fill: parent
                            color: "black"
                            opacity: win.turnKind === "flip" && win.turnDirection < 0 ? 0.4 * (1 - win.turnProgress)
                                   : win.turnKind === "cover" && win.turnDirection < 0 ? 0.25 * (1 - win.turnProgress) : 0
                            visible: opacity > 0
                        }
                    }

                    // The picture of the page being left. Between turns it waits
                    // out of sight, where it can still be drawn into.
                    ShaderEffectSource {
                        id: snapshot
                        objectName: "snapshot"
                        sourceItem: stage
                        live: false
                        width: stage.width
                        height: stage.height
                        x: win.turnKind === "" ? -width - 64
                         : win.turnKind === "slide" ? -win.turnDirection * width * win.turnProgress
                         : win.turnKind === "cover" && win.turnDirection < 0 ? width * win.turnProgress : 0
                        z: stage.front ? 0 : 1
                        textureSize: Qt.size(Math.ceil(width * Screen.devicePixelRatio), Math.ceil(height * Screen.devicePixelRatio))
                        opacity: win.turnKind === "fade" ? Math.max(0, 1 - 2 * win.turnProgress) : 1
                        transform: Rotation {
                            // Turning forward, the page swings away about the spine.
                            origin.x: 0
                            origin.y: snapshot.height / 2
                            distanceToPlane: 3 * snapshot.width
                            axis { x: 0; y: 1; z: 0 }
                            angle: win.turnKind === "flip" && win.turnDirection > 0 ? -90 * win.turnProgress : 0
                        }

                        Rectangle {
                            anchors.fill: parent
                            color: "black"
                            opacity: win.turnKind === "flip" && win.turnDirection > 0 ? 0.4 * win.turnProgress
                                   : win.turnKind === "cover" && win.turnDirection > 0 ? 0.25 * win.turnProgress : 0
                            visible: opacity > 0
                        }
                    }
                }

                // The pages after these, drawn ahead so turning to them is instant.
                Repeater {
                    model: backend.paged ? win.shownPages.map(page => page + win.shownPages.length)
                                                       .filter(page => page < backend.sectionCount) : []
                    delegate: Image {
                        required property int modelData
                        visible: false
                        asynchronous: true
                        source: "image://omaread/page/" + backend.bookId + "/" + modelData + win.tint
                        sourceSize: Qt.size(Math.round(spreadRow.pageHeight * win.aspect(modelData) * Screen.devicePixelRatio),
                                            Math.round(spreadRow.pageHeight * Screen.devicePixelRatio))
                    }
                }

                Binding {
                    target: backend
                    property: "readingWidth"
                    value: prose.width
                }
                Binding {
                    target: backend
                    property: "readingSize"
                    value: win.readingSize
                }

                // The matches, each in the words around it, to choose from. The
                // current one is the one the text shows.
                Rectangle {
                    id: findResults
                    objectName: "findResults"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: findBar.top
                    height: Math.min(hitRows.contentHeight, Math.round(win.uiSize * 3.6) * 5) + 8
                    visible: win.finding && backend.hitCount > 0 && !backend.searching
                    color: theme.background
                    border.color: theme.colors.selection

                    ListView {
                        id: hitRows
                        objectName: "hitRows"
                        anchors.fill: parent
                        anchors.margins: 4
                        clip: true
                        model: backend.hitList
                        currentIndex: backend.hitIndex < count ? backend.hitIndex : -1
                        highlightMoveDuration: 0
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)

                        delegate: Rectangle {
                            id: hitRow
                            required property int index
                            required property var modelData
                            width: ListView.view.width
                            height: Math.round(win.uiSize * 3.6)
                            radius: Math.min(4, theme.radius)
                            color: ListView.isCurrentItem ? theme.colors.selection : "transparent"
                            function escaped(text) { return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;") }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 0
                                Label {
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignLeft
                                    text: backend.paged ? qsTr("Page %1").arg(win.number(hitRow.modelData.section + 1)) : hitRow.modelData.chapter
                                    color: win.muted
                                    font.pixelSize: Math.round(win.uiSize * 6 / 7)
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    horizontalAlignment: Text.AlignLeft
                                    visible: hitRow.modelData.context !== ""
                                    textFormat: Text.StyledText
                                    text: {
                                        const c = hitRow.modelData.context, at = hitRow.modelData.at, n = hitRow.modelData.length
                                        return hitRow.escaped(c.slice(0, at)) + "<b>" + hitRow.escaped(c.slice(at, at + n)) + "</b>"
                                               + hitRow.escaped(c.slice(at + n))
                                    }
                                    elide: Text.ElideRight
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: backend.chooseHit(hitRow.index)
                            }
                        }
                    }
                }

                // Searching the book: Enter for the next match, Shift+Enter for
                // the one before, Escape to stop.
                Rectangle {
                    id: findBar
                    objectName: "findBar"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: statusBar.top
                    height: findRow.implicitHeight + 16
                    visible: win.finding
                    color: theme.colors.selection

                    RowLayout {
                        id: findRow
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        spacing: 16

                        TextField {
                            id: findField
                            objectName: "findField"
                            Layout.fillWidth: true
                            Layout.maximumWidth: 480
                            placeholderText: qsTr("Find in the book")
                            Material.roundedScale: theme.radius
                            function submit(backwards) {
                                if (text.trim() === "")
                                    return
                                if (backend.query === text.trim().replace(/\s+/g, " ") && !backend.searching)
                                    backwards ? backend.previousHit() : backend.nextHit()
                                else
                                    backend.find(text)
                            }
                            Keys.onReturnPressed: event => submit(event.modifiers & Qt.ShiftModifier)
                            Keys.onEnterPressed: event => submit(event.modifiers & Qt.ShiftModifier)
                            Keys.onEscapePressed: win.stopFinding()
                            // Up and Down move through the matches, as the list shows them.
                            Keys.onUpPressed: if (backend.hitCount > 0) backend.previousHit()
                            Keys.onDownPressed: if (backend.hitCount > 0) backend.nextHit()
                        }
                        Label {
                            objectName: "findStatus"
                            Layout.fillWidth: true
                            color: win.muted
                            elide: Text.ElideRight
                            text: backend.searching ? qsTr("Searching…")
                                : backend.query === "" ? qsTr("Enter to search")
                                : backend.hitCount === 0 ? qsTr("No matches")
                                : qsTr("%1 of %2").arg(win.number(backend.hitIndex + 1)).arg(win.number(backend.hitCount))
                        }
                    }
                }

                RowLayout {
                    id: statusBar
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    height: Math.round(win.uiSize * 2.3)
                    spacing: 16

                    Label {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 2
                        // Explicit, so mirroring moves it to the right in a
                        // right-to-left language whatever the title's own script.
                        horizontalAlignment: Text.AlignLeft
                        text: backend.title
                        color: win.muted
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 2
                        horizontalAlignment: Text.AlignHCenter
                        objectName: "chapter"
                        text: backend.chapterLabel
                        color: win.muted
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "where"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 2
                        horizontalAlignment: Text.AlignRight
                        text: win.whereText()
                        color: win.muted
                        elide: Text.ElideLeft
                        // A click steps through what it shows, as P does.
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: win.nextProgress()
                        }
                    }
                }
            }
        }

        // Drop a file anywhere to read it.
        DropArea {
            anchors.fill: parent
            onDropped: drop => { if (drop.hasUrls) backend.openUrl(drop.urls[0]) }
        }

        BusyIndicator {
            anchors.centerIn: parent
            running: backend.loading
            visible: running
        }

        // What went wrong, in the window, until dismissed or something opens.
        Rectangle {
            objectName: "error"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 56
            width: Math.min(parent.width - 48, errorText.implicitWidth + 48)
            height: errorText.implicitHeight + 24
            visible: backend.error !== ""
            radius: Math.min(6, theme.radius)
            color: theme.colors.selection
            border.color: theme.colors.red || theme.accent

            Label {
                id: errorText
                anchors.fill: parent
                anchors.margins: 12
                anchors.leftMargin: 24
                anchors.rightMargin: 24
                text: backend.error
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            MouseArea {
                anchors.fill: parent
                onClicked: backend.dismissError()
            }
        }

        // A short word that something happened, such as a copy.
        Rectangle {
            objectName: "toast"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 56
            width: toastText.implicitWidth + 32
            height: toastText.implicitHeight + 16
            visible: win.toast !== "" && backend.error === ""
            radius: Math.min(6, theme.radius)
            color: theme.colors.selection

            Label {
                id: toastText
                anchors.centerIn: parent
                text: win.toast
            }
        }
    }

    // How a flowing book's text is set. It sits at the bottom without dimming
    // the page, so each change shows on the text behind it. Up and Down choose
    // a setting, Left and Right change it, and S or Escape close it.
    Popup {
        id: settings
        objectName: "settings"
        x: Math.round((win.width - width) / 2)
        // Over the footer, so no half-line peeks out beneath it.
        y: win.height - height - 8
        width: Math.min(620, win.width - 32)
        modal: false
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 16
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }

        property int row: 0
        readonly property var rows: [
            { key: "typeface", label: qsTr("Typeface"),
              options: [{ value: "serif", label: qsTr("Serif") }, { value: "sans", label: qsTr("Sans") }] },
            { key: "size", label: qsTr("Text size"),
              options: [{ value: -1, label: "A−" }, { value: 0, label: win.percent(backend.textScale / 1.5) }, { value: 1, label: "A+" }] },
            { key: "lineSpacing", label: qsTr("Line spacing"),
              options: [{ value: "compact", label: qsTr("Compact") }, { value: "normal", label: qsTr("Normal") },
                        { value: "relaxed", label: qsTr("Relaxed") }] },
            { key: "columnWidth", label: qsTr("Column"),
              options: [{ value: "narrow", label: qsTr("Narrow") }, { value: "medium", label: qsTr("Medium") },
                        { value: "wide", label: qsTr("Wide") }] },
            { key: "justify", label: qsTr("Alignment"),
              options: [{ value: false, label: qsTr("Ragged") }, { value: true, label: qsTr("Justified") }] }
        ]
        function current(key) { return key === "size" ? 0 : backend[key] }
        function choose(key, value) {
            if (key === "size")
                win.resize(value)
            else
                backend[key] = value
        }
        // Left and Right step through a row's choices; text size grows and shrinks.
        function step(by) {
            const r = rows[row]
            if (r.key === "size")
                return choose("size", by)
            const at = r.options.findIndex(o => o.value === current(r.key))
            const next = Math.max(0, Math.min(r.options.length - 1, at + by))
            choose(r.key, r.options[next].value)
        }
        // As it starts to appear, so the keys go to it during its opening animation.
        onAboutToShow: { row = 0; panel.forceActiveFocus() }

        FocusScope {
            id: panel
            implicitWidth: settingsColumn.implicitWidth
            implicitHeight: settingsColumn.implicitHeight
            anchors.fill: parent
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            Keys.onUpPressed: settings.row = Math.max(0, settings.row - 1)
            Keys.onDownPressed: settings.row = Math.min(settings.rows.length - 1, settings.row + 1)
            // Mirrored rows run the other way, so the arrow keys follow what's seen.
            Keys.onLeftPressed: settings.step(win.rightToLeft ? 1 : -1)
            Keys.onRightPressed: settings.step(win.rightToLeft ? -1 : 1)
            // The panel has the keyboard, so the window's S shortcut doesn't reach it.
            Keys.onPressed: event => { if (event.key === Qt.Key_S) { settings.close(); event.accepted = true } }

            ColumnLayout {
                id: settingsColumn
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 4

                Repeater {
                    model: settings.rows
                    delegate: Rectangle {
                        id: settingRow
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: Math.round(win.uiSize * 2.7)
                        radius: Math.min(4, theme.radius)
                        color: settings.row === index ? Qt.rgba(theme.colors.selection.r, theme.colors.selection.g,
                                                                  theme.colors.selection.b, 0.5) : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                text: settingRow.modelData.label
                                color: settings.row === settingRow.index ? theme.foreground : win.muted
                                elide: Text.ElideRight
                            }
                            Repeater {
                                model: settingRow.modelData.options
                                delegate: Rectangle {
                                    id: choice
                                    required property var modelData
                                    readonly property bool isSize: settingRow.modelData.key === "size"
                                    readonly property bool chosen: !isSize && settings.current(settingRow.modelData.key) === modelData.value
                                    readonly property bool readout: isSize && modelData.value === 0
                                    implicitWidth: Math.max(Math.round(win.uiSize * 4.2), choiceLabel.implicitWidth + 24)
                                    implicitHeight: Math.round(win.uiSize * 2)
                                    radius: Math.min(4, theme.radius)
                                    color: chosen ? theme.accent : "transparent"
                                    border.color: chosen || readout ? "transparent" : theme.colors.selection
                                    Label {
                                        id: choiceLabel
                                        anchors.centerIn: parent
                                        text: choice.modelData.label
                                        color: choice.chosen ? theme.accentForeground : theme.foreground
                                        font.family: settingRow.modelData.key === "typeface" && choice.modelData.value === "serif"
                                                     ? "Noto Serif" : win.font.family
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: !choice.readout
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            settings.row = settingRow.index
                                            settings.choose(settingRow.modelData.key, choice.modelData.value)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("↑ ↓ to choose  ·  ← → to change  ·  S to close")
                    color: win.muted
                    font.pixelSize: Math.round(win.uiSize * 6 / 7)
                }
            }
        }
    }

    // A footnote or endnote, shown over the bottom of the page where its
    // number was chosen, so reading never leaves the page. Enter goes to the
    // note itself; Escape or a click outside closes it.
    Popup {
        id: note
        objectName: "note"
        property string html: ""
        property string link: ""
        function show(text, at) {
            html = text
            link = at
            open()
        }
        function go() {
            close()
            if (backend.followLink(link))
                win.hintBack()
        }
        x: Math.round((win.width - width) / 2)
        y: win.height - height - 8
        width: Math.min(680, win.width - 32)
        modal: false
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 20
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }
        onAboutToShow: { noteView.contentY = 0; notePanel.forceActiveFocus() }

        FocusScope {
            id: notePanel
            implicitWidth: noteColumn.implicitWidth
            implicitHeight: noteColumn.implicitHeight
            anchors.fill: parent
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            Keys.onReturnPressed: note.go()
            Keys.onEnterPressed: note.go()
            Keys.onUpPressed: noteView.contentY = Math.max(0, noteView.contentY - 40)
            Keys.onDownPressed: noteView.contentY = Math.max(0, Math.min(noteView.contentHeight - noteView.height, noteView.contentY + 40))

            ColumnLayout {
                id: noteColumn
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 12

                Flickable {
                    id: noteView
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(noteText.height, Math.round(win.height * 0.4))
                    contentHeight: noteText.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    TextEdit {
                        id: noteText
                        objectName: "noteText"
                        width: noteView.width
                        text: note.html
                        textFormat: TextEdit.RichText
                        readOnly: true
                        selectByMouse: true
                        activeFocusOnPress: false
                        wrapMode: TextEdit.Wrap
                        font.pixelSize: Math.round(win.readingSize * 0.9)
                        font.family: prose.font.family
                        color: theme.foreground
                        selectionColor: theme.accent
                        selectedTextColor: theme.accentForeground
                        onTextChanged: {
                            backend.loadImages(textDocument)
                            backend.typeset(textDocument, win.lineHeight, 0, false)
                        }
                        onLinkActivated: link => { note.close(); win.openLink(link) }
                        HoverHandler {
                            cursorShape: noteText.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Esc to close")
                        color: win.muted
                        font.pixelSize: Math.round(win.uiSize * 6 / 7)
                        elide: Text.ElideRight
                    }
                    Button {
                        objectName: "goToNote"
                        text: qsTr("Go to note")
                        flat: true
                        Material.roundedScale: theme.radius
                        onClicked: note.go()
                    }
                }
            }
        }
    }

    // The table of contents: Up and Down to choose, Enter to go there.
    Popup {
        id: contents
        objectName: "contents"
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, win.width - 64)
        height: Math.min(contentsColumn.implicitHeight + topPadding + bottomPadding, win.height - 96)
        modal: true
        focus: true
        padding: 20
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }
        onOpened: {
            chapters.currentIndex = Math.max(0, backend.tocIndex)
            chapters.positionViewAtIndex(chapters.currentIndex, ListView.Center)
            chapters.forceActiveFocus()
        }

        function choose(index) {
            backend.goToEntry(index)
            win.hintBack()
            Qt.callLater(win.restore)
            close()
        }

        ColumnLayout {
            id: contentsColumn
            anchors.fill: parent
            spacing: 12
            // A popup doesn't inherit the window's mirroring.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label {
                Layout.fillWidth: true
                text: qsTr("Contents")
                font.pixelSize: Math.round(win.uiSize * 10 / 7)
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                visible: chapters.count === 0
                text: qsTr("This book has no table of contents. Use [ and ] to move between sections.")
                color: win.muted
                wrapMode: Text.Wrap
            }
            ListView {
                id: chapters
                objectName: "chapters"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: contentHeight
                visible: count > 0
                clip: true
                model: backend.toc
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar {}
                Keys.onReturnPressed: contents.choose(currentIndex)
                Keys.onEnterPressed: contents.choose(currentIndex)

                delegate: Rectangle {
                    id: chapter
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    height: Math.round(win.uiSize * 2.6)
                    radius: Math.min(4, theme.radius)
                    color: ListView.isCurrentItem ? theme.colors.selection : "transparent"

                    Label {
                        anchors.fill: parent
                        anchors.leftMargin: 12 + 18 * Math.min(chapter.modelData.depth, 4)
                        anchors.rightMargin: 12
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignLeft
                        text: chapter.modelData.title
                        font.bold: chapter.index === backend.tocIndex
                        color: chapter.index === backend.tocIndex ? theme.accent : theme.foreground
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: contents.choose(chapter.index)
                    }
                }
            }
        }
    }

    // Go to a page or section by its number.
    Dialog {
        id: goDialog
        objectName: "goDialog"
        anchors.centerIn: Overlay.overlay
        modal: true
        title: backend.paged ? qsTr("Go to page") : qsTr("Go to section")
        Material.roundedScale: theme.radius
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }
        onOpened: { pageField.text = ""; pageField.forceActiveFocus() }
        onAccepted: {
            const n = parseInt(pageField.text)
            if (n >= 1 && n <= backend.sectionCount) {
                win.markJump()
                win.jump(n - 1, 0)
            }
        }

        TextField {
            id: pageField
            objectName: "pageField"
            implicitWidth: 280
            LayoutMirroring.enabled: win.rightToLeft
            placeholderText: qsTr("1 to %1").arg(win.number(backend.sectionCount))
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator { bottom: 1; top: Math.max(1, backend.sectionCount) }
            Material.roundedScale: theme.radius
            onAccepted: goDialog.accept()
        }

        footer: DialogButtonBox {
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            background: null
            Button { text: qsTr("Cancel"); flat: true; Material.roundedScale: theme.radius; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            Button { text: qsTr("Go"); flat: true; Material.roundedScale: theme.radius; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        }
    }

    // A locked PDF asks for its password, which is never kept.
    Dialog {
        id: passwordDialog
        objectName: "passwordDialog"
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Password")
        Material.roundedScale: theme.radius
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }
        onOpened: { passwordField.text = ""; passwordField.forceActiveFocus() }
        onAccepted: backend.unlock(passwordField.text)
        onRejected: backend.cancelPassword()

        ColumnLayout {
            spacing: 12
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Label {
                Layout.fillWidth: true
                Layout.preferredWidth: 340
                text: backend.passwordWrong ? qsTr("That password didn't open %1. Try again.").arg(backend.passwordFor)
                                            : qsTr("%1 is locked with a password.").arg(backend.passwordFor)
                color: backend.passwordWrong ? (theme.colors.red || theme.foreground) : theme.foreground
                wrapMode: Text.Wrap
            }
            TextField {
                id: passwordField
                objectName: "passwordField"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: qsTr("Password")
                Material.roundedScale: theme.radius
                onAccepted: passwordDialog.accept()
            }
        }

        footer: DialogButtonBox {
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            background: null
            Button { text: qsTr("Cancel"); flat: true; Material.roundedScale: theme.radius; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            Button { text: qsTr("Open"); flat: true; Material.roundedScale: theme.radius; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
        }
    }

    Popup {
        id: help
        objectName: "help"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 24
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }

        ColumnLayout {
            spacing: 6
            // A popup is not an Item and doesn't inherit the window's
            // mirroring, so its content opts in.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Repeater {
                model: [
                    { keys: "O", action: qsTr("Open a book or document") },
                    { keys: "Space  →  PgDn", action: qsTr("Next screen or page") },
                    { keys: "Shift+Space  ←  PgUp", action: qsTr("Previous screen or page") },
                    { keys: "↓ ↑  J K", action: qsTr("Scroll") },
                    { keys: "]  [", action: qsTr("Next or previous section") },
                    { keys: "Home  End", action: qsTr("First or last section") },
                    { keys: "G", action: qsTr("Go to a page or section") },
                    { keys: "T", action: qsTr("Table of contents") },
                    { keys: "/  Ctrl+F", action: qsTr("Find in the book") },
                    { keys: "N  Shift+N", action: qsTr("Next or previous match") },
                    { keys: "Ctrl+C", action: qsTr("Copy the selected words") },
                    { keys: "+  −  0", action: qsTr("Larger, smaller, or reset text and pages") },
                    { keys: "W", action: qsTr("Fit pages to the width or the window") },
                    { keys: "D", action: qsTr("One page or two side by side") },
                    { keys: "I", action: qsTr("Color pages like the theme") },
                    { keys: "Tab  Enter", action: qsTr("Choose a link or note on the page, and open it") },
                    { keys: "Backspace  Shift+Backspace", action: qsTr("Back or forward after a jump") },
                    { keys: "P", action: qsTr("Show time left, the page, or the percent read") },
                    { keys: "S", action: qsTr("Reading settings: typeface, spacing, column") },
                    { keys: "A", action: qsTr("Change how pages turn") },
                    { keys: "F", action: qsTr("Full screen") },
                    { keys: "Esc", action: qsTr("Back to the library") },
                    { keys: "Enter  Delete", action: qsTr("Open or forget a book in the library") },
                    { keys: "?", action: qsTr("Show or hide the shortcuts") },
                    { keys: "Q", action: qsTr("Quit") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    spacing: 24

                    Label {
                        Layout.preferredWidth: win.uiSize * 12
                        text: modelData.keys
                        color: theme.accent
                        font.bold: true
                    }
                    Label { text: modelData.action }
                }
            }
        }
    }
}
