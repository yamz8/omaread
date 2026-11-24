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
    readonly property bool busy: help.opened || contents.visible || skim.visible || passwordDialog.opened
                                 // From the moment it appears, not once its opening animation ends,
                                 // or a key pressed right after S turns the page behind it.
                                 || settings.visible || note.visible || selectionBar.visible || noteEditor.visible
                                 || lookup.visible
                                 || findField.activeFocus || shelfSearch.activeFocus
    readonly property bool reading: backend.open && !busy && settled
    readonly property bool browsing: !backend.open && !busy && settled
    readonly property bool shelfKeys: browsing && !shelfSearch.activeFocus

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

    readonly property int step: backend.paged && backend.spread && !panelView ? 2 : 1
    // A paged book read right to left, as manga are: pages side by side and
    // page turns go the other way.
    readonly property bool bookRightToLeft: backend.paged && backend.rightToLeft
    // Which way a turn moves across the screen: forward is leftward, unless
    // the book reads the other way.
    readonly property int turnSide: turnDirection * (bookRightToLeft ? -1 : 1)
    readonly property var shownPages: !backend.paged ? []
        : backend.spread && !panelView && backend.section + 1 < backend.sectionCount ? [backend.section, backend.section + 1]
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
    // A page's width over its height: the whole page's, or, with the margins
    // trimmed, its content's. Panel view always works on whole pages.
    function fullAspect(page) { return backend.aspects[page] || 0.7071 }
    function crop(page) {
        return backend.trimMargins && !panelView && backend.paged ? backend.contentBox(page) : Qt.rect(0, 0, 1, 1)
    }
    function aspect(page) {
        const c = crop(page)
        return fullAspect(page) * c.width / c.height
    }
    // Zooms a PDF's or comic's pages around a point of the view, keeping what
    // is under it there: the pointer's place for a wheel, a pinch's middle.
    function zoomAt(factor, px, py) {
        const before = zoom, after = Math.max(0.5, Math.min(6, zoom * factor))
        if (Math.abs(after - before) < 0.001)
            return
        const cx = view.contentX + px, cy = view.contentY + py
        const oldX = spreadRow.x, oldY = spreadRow.y, oldWidth = spreadRow.width, oldHeight = spreadRow.height
        restoring = true
        zoom = after
        // The pages laid out at the new size now, not at the next frame, so
        // the view is placed against where they really are.
        spreadRow.forceLayout()
        const kx = spreadRow.width / oldWidth, ky = spreadRow.height / oldHeight
        view.contentX = Math.max(0, Math.min(spreadRow.x + (cx - oldX) * kx - px, Math.max(0, view.contentWidth - view.width)))
        view.contentY = Math.max(0, Math.min(spreadRow.y + (cy - oldY) * ky - py, Math.max(0, view.contentHeight - view.height)))
        restoring = false
        backend.position = view.contentHeight > view.height ? view.contentY / (view.contentHeight - view.height) : 0
    }

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
    // Panel view: a comic read a panel at a time, each zoomed to fill the
    // window, then on to the next page's first panel. A page without panels
    // is shown whole.
    property bool panelView: false
    property int panelIndex: 0
    property bool panelFromEnd: false
    readonly property var panels: {
        void backend.panelsFound
        return panelView && backend.paged ? backend.panelsOf(backend.section) : []
    }
    onPanelsChanged: if (panelView) {
        if (panelFromEnd)
            panelIndex = Math.max(0, panels.length - 1)
        panelFromEnd = false
        panelIndex = Math.min(panelIndex, Math.max(0, panels.length - 1))
        framePanel()
    }
    function setPanelView(on) {
        panelView = on && backend.paged
        panelIndex = 0
        if (!panelView) {
            zoom = 1
            Qt.callLater(restore)
        } else {
            framePanel()
        }
        say(panelView ? qsTr("Panel by panel") : qsTr("Whole pages"))
    }
    // Zooms the view onto the panel, as large as fits with a little room around.
    function framePanel() {
        if (!panelView)
            return
        if (panels.length === 0) {
            zoom = 1
            view.contentX = Math.max(0, (view.contentWidth - view.width) / 2)
            view.contentY = 0
            return
        }
        const r = panels[panelIndex], a = aspect(backend.section), pad = 24, m = spreadRow.margin
        const fitted = spreadRow.fitted
        const z = Math.max(1, Math.min(8, Math.min((view.width - 2 * pad) / (r.width * fitted * a),
                                                     (view.height - 2 * pad) / (r.height * fitted))))
        restoring = true
        zoom = z
        const ph = fitted * z, pw = ph * a
        const rowX = Math.max(m, (view.width - pw) / 2)
        const cx = rowX + (r.x + r.width / 2) * pw, cy = m + (r.y + r.height / 2) * ph
        view.contentX = Math.max(0, Math.min(cx - view.width / 2, Math.max(0, pw + 2 * m - view.width)))
        view.contentY = Math.max(0, Math.min(cy - view.height / 2, Math.max(0, ph + 2 * m - view.height)))
        restoring = false
    }
    function forward() {
        letLinkGo()
        noteScreenRead()
        if (panelView && panels.length > 0 && panelIndex < panels.length - 1) {
            turn(1, () => { ++panelIndex; framePanel() })
            return
        }
        if (panelView) {
            panelIndex = 0
            if (backend.section + 1 < backend.sectionCount)
                jump(backend.section + 1, 0)
            return
        }
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
        if (panelView && panels.length > 0 && panelIndex > 0) {
            turn(-1, () => { --panelIndex; framePanel() })
            return
        }
        if (panelView) {
            if (backend.section > 0) {
                panelFromEnd = true
                panelIndex = 0
                jump(backend.section - 1, 0)
            }
            return
        }
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
    // The library's books as shown: the filter chosen, the search typed, and
    // the order chosen.
    function inFilter(book, filter) {
        return filter === "reading" ? book.started && !book.finished
             : filter === "unread" ? !book.started && !book.finished
             : filter === "finished" ? book.finished : true
    }
    function countIn(filter) { return backend.library.filter(b => inFilter(b, filter)).length }
    readonly property var shelf: {
        const words = shelfSearch.text.trim().toLowerCase()
        const collator = (a, b) => a.localeCompare(b, Qt.locale().name, { numeric: true, sensitivity: "base" })
        const books = backend.library.filter(b => inFilter(b, backend.libraryFilter)
            && (words === "" || b.title.toLowerCase().includes(words) || (b.author || "").toLowerCase().includes(words)))
        const sort = backend.librarySort
        return books.sort((a, b) => sort === "title" ? collator(a.title, b.title)
                                  : sort === "author" ? collator(a.author || "￿", b.author || "￿") || collator(a.title, b.title)
                                  : sort === "progress" ? b.progress - a.progress
                                  : b.recent - a.recent)
    }
    readonly property var shelfView: backend.libraryView === "grid" ? shelfGrid : shelfList
    function sortName(sort) {
        return sort === "title" ? qsTr("Title") : sort === "author" ? qsTr("Author")
             : sort === "progress" ? qsTr("Progress") : qsTr("Recent")
    }
    function nextSort() {
        const sorts = ["recent", "title", "author", "progress"]
        backend.librarySort = sorts[(sorts.indexOf(backend.librarySort) + 1) % sorts.length]
        say(qsTr("In order of: %1").arg(sortName(backend.librarySort)))
    }
    function nextFilter(step) {
        const filters = ["all", "reading", "unread", "finished"]
        backend.libraryFilter = filters[(filters.indexOf(backend.libraryFilter) + step + filters.length) % filters.length]
    }
    // The book chosen, kept within the ones shown as a search or filter narrows them.
    function shelfBook() { return shelf[Math.max(0, Math.min(shelfView.currentIndex, shelf.length - 1))] }
    function openShelf() {
        const book = shelfBook()
        if (book)
            backend.openFile(book.path)
    }
    // Arrows move through the covers as they're laid out, in any direction.
    function moveShelf(key) {
        const view = shelfView
        if (view === shelfGrid) {
            const forward = (key === Qt.Key_Right) !== rightToLeft
            if (key === Qt.Key_Left || key === Qt.Key_Right)
                view.currentIndex = Math.max(0, Math.min(view.count - 1, view.currentIndex + (forward ? 1 : -1)))
            else
                view.currentIndex = Math.max(0, Math.min(view.count - 1, view.currentIndex + (key === Qt.Key_Down ? 1 : -1) * view.columns))
        } else if (key === Qt.Key_Down || key === Qt.Key_Up) {
            view.currentIndex = Math.max(0, Math.min(view.count - 1, view.currentIndex + (key === Qt.Key_Down ? 1 : -1)))
        }
        view.positionViewAtIndex(view.currentIndex, GridView.Contain)
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
        if (panelView && panels.length > 1)
            return qsTr("Panel %1 of %2").arg(number(panelIndex + 1)).arg(number(panels.length)) + "  ·  "
                   + qsTr("Page %1 of %2").arg(number(backend.section + 1)).arg(number(backend.sectionCount))
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
        // In panel view, the place is the panel.
        if (panelView) {
            framePanel()
            return
        }
        restoring = true
        view.contentY = backend.position * Math.max(0, view.contentHeight - view.height)
        // Zoomed in on a page, the reader's place across it stays.
        view.contentX = backend.paged && zoom > 1 ? Math.max(0, Math.min(view.contentX, view.contentWidth - view.width))
                                                  : Math.max(0, (view.contentWidth - view.width) / 2)
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
                const c = crop(hit.page)
                const y = spreadRow.y + (hit.y - c.y) / c.height * sheet.height
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
    // The page on view, as positions in its section, for a bookmark: a
    // bookmark is on the page when it lies between them.
    function bookmarkRange() {
        if (backend.paged)
            return { from: 0, to: 1.0001 }
        const max = view.contentHeight - view.height
        if (max <= 0)
            return { from: 0, to: 1.0001 }
        return { from: view.contentY / max, to: Math.min(1.0001, (view.contentY + view.height * 0.9) / max) }
    }
    readonly property bool bookmarkHere: {
        void view.contentY
        const range = bookmarkRange()
        return backend.bookmarks.some(b => b.section === backend.section && b.position >= range.from - 0.0001
                                           && b.position < range.to)
    }
    function toggleBookmark() {
        const range = bookmarkRange()
        let text = qsTr("Page %1").arg(number(backend.section + 1))
        if (!backend.paged) {
            const start = prose.positionAt(0, view.contentY - prose.y + lineCover.top + 2)
            text = prose.getText(start, Math.min(prose.length, start + 200))
        }
        say(backend.toggleBookmark(range.from, range.to, text) ? qsTr("Bookmarked") : qsTr("Bookmark removed"))
    }

    // Highlights, in the theme's own colors: full for marks beside them, and
    // laid over the page's color for the words themselves, so text reads on them.
    function highlightInk(name) {
        const key = name === "pink" ? "magenta" : name
        return theme.colors[key] || theme.accent
    }
    readonly property var highlightPaint: {
        const paint = {}
        for (const name of ["yellow", "green", "blue", "pink"]) {
            const c = highlightInk(name)
            paint[name] = Qt.tint(theme.background, Qt.rgba(c.r, c.g, c.b, theme.light ? 0.45 : 0.38))
        }
        return paint
    }
    property string lastHighlightColor: "yellow"
    function paintHighlights() { backend.paintHighlights(prose.textDocument, highlightPaint) }
    // A selection made with the mouse, or a highlight clicked: the bar offers
    // what can be done with it.
    function showSelectionBar(existing) {
        selectionBar.highlight = existing
        if (existing >= 0) {
            const h = backend.highlights[existing]
            selectionBar.start = h.start
            selectionBar.end = h.end
        } else {
            selectionBar.start = prose.selectionStart
            selectionBar.end = prose.selectionEnd
        }
        const r = prose.positionToRectangle(selectionBar.start)
        const at = prose.mapToItem(Overlay.overlay, r.x, r.y)
        selectionBar.anchorY = at.y
        selectionBar.anchorX = prose.mapToItem(Overlay.overlay, prose.width / 2, 0).x
        selectionBar.open()
    }
    function highlightSelection(color) {
        lastHighlightColor = color
        if (selectionBar.highlight >= 0) {
            backend.setHighlightColor(selectionBar.highlight, color)
        } else if (prose.selectionEnd > prose.selectionStart) {
            selectionBar.highlight = backend.addHighlight(prose.selectionStart, prose.selectionEnd, prose.selectedText, color)
            prose.deselect()
        }
        return selectionBar.highlight
    }
    function lookUpSelection() {
        const word = selectionBar.highlight >= 0 ? backend.highlights[selectionBar.highlight].text : prose.selectedText
        selectionBar.close()
        lookup.ask(word.trim().split(/\s+/).slice(0, 4).join(" "))
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
        else if (panelView)
            setPanelView(false)
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
        function onBookChanged() { win.restoring = true; win.panelView = false; win.zoom = 1; win.finding = false; win.screenStarted = Date.now(); Qt.callLater(win.restore) }
        function onSectionChanged() { win.restoring = true; win.clearSelections(); Qt.callLater(win.restore) }
        // New text, for a new section or a new text size: its layout moves the
        // view, which mustn't move the reader's place.
        // The end of the book: said once, as the library marks it finished.
        function onBookFinished() { win.say(qsTr("Finished: %1").arg(backend.title)) }
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

    Shortcut { sequences: ["Space", "PgDown"]; enabled: win.reading; onActivated: win.forward() }
    Shortcut { sequences: ["Shift+Space", "PgUp"]; enabled: win.reading; onActivated: win.backward() }
    // The arrows turn the way the book reads: a manga's next page is to the left.
    Shortcut { sequence: "Right"; enabled: win.reading; onActivated: win.bookRightToLeft ? win.backward() : win.forward() }
    Shortcut { sequence: "Left"; enabled: win.reading; onActivated: win.bookRightToLeft ? win.forward() : win.backward() }
    Shortcut { sequence: "V"; enabled: win.reading && backend.paged; onActivated: win.setPanelView(!win.panelView) }
    Shortcut {
        sequence: "C"
        enabled: win.reading && backend.paged
        onActivated: {
            backend.trimMargins = !backend.trimMargins
            win.say(backend.trimMargins ? qsTr("Margins trimmed") : qsTr("Whole pages"))
            Qt.callLater(win.restore)
        }
    }
    Shortcut {
        sequence: "R"
        enabled: win.reading && backend.paged
        onActivated: {
            backend.rightToLeft = !backend.rightToLeft
            win.say(backend.rightToLeft ? qsTr("Read right to left") : qsTr("Read left to right"))
        }
    }
    Shortcut { sequences: ["Down", "J"]; enabled: win.reading; onActivated: win.scroll(60) }
    Shortcut { sequences: ["Up", "K"]; enabled: win.reading; onActivated: win.scroll(-60) }
    Shortcut { sequence: "]"; enabled: win.reading; onActivated: win.jump(backend.section + win.step, 0) }
    Shortcut { sequence: "["; enabled: win.reading; onActivated: win.jump(Math.max(0, backend.section - win.step), 0) }
    Shortcut { sequence: "Home"; enabled: win.reading; onActivated: { win.markJump(); win.jump(0, 0) } }
    Shortcut { sequence: "End"; enabled: win.reading; onActivated: { win.markJump(); win.jump(backend.sectionCount - 1, 0) } }
    Shortcut { sequence: "B"; enabled: win.reading; onActivated: win.toggleBookmark() }
    // Highlights the words chosen with the mouse, in the color used last.
    Shortcut {
        sequence: "H"
        enabled: win.reading && !backend.paged && prose.selectedText.trim() !== ""
        onActivated: {
            win.highlightSelection(win.lastHighlightColor)
            win.say(qsTr("Highlighted"))
        }
    }
    Shortcut { sequence: "Tab"; enabled: win.reading && !backend.paged; onActivated: win.chooseLink(1) }
    Shortcut { sequences: ["Backtab", "Shift+Backtab", "Shift+Tab"]; enabled: win.reading && !backend.paged; onActivated: win.chooseLink(-1) }
    Shortcut { sequences: ["Return", "Enter"]; enabled: win.reading && win.chosenLink !== null; onActivated: win.openChosenLink() }
    Shortcut { sequences: ["Backspace", "Alt+Left"]; enabled: win.reading && backend.canGoBack; onActivated: win.goBack() }
    Shortcut { sequences: ["Shift+Backspace", "Alt+Right"]; enabled: win.reading && backend.canGoForward; onActivated: win.goForward() }
    Shortcut { sequence: "T"; enabled: backend.open && (!win.busy || contents.visible); onActivated: contents.visible ? contents.close() : contents.open() }
    Shortcut { sequence: "G"; enabled: win.reading; onActivated: skim.open() }
    Shortcut { sequences: ["/", StandardKey.Find]; enabled: win.shelfKeys && backend.library.length > 0; onActivated: shelfSearch.forceActiveFocus() }
    Shortcut { sequences: ["/", StandardKey.Find]; enabled: backend.open && settled && !help.opened && !contents.visible && !skim.visible; onActivated: win.startFinding() }
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

    // The library: arrows through the books, Enter to open, Delete to
    // forget, M to mark read, S for the order, V for covers or a list, Tab
    // for which books, / to search.
    Shortcut { sequence: "Down"; enabled: win.shelfKeys; onActivated: win.moveShelf(Qt.Key_Down) }
    Shortcut { sequence: "Up"; enabled: win.shelfKeys; onActivated: win.moveShelf(Qt.Key_Up) }
    Shortcut { sequence: "Left"; enabled: win.shelfKeys; onActivated: win.moveShelf(Qt.Key_Left) }
    Shortcut { sequence: "Right"; enabled: win.shelfKeys; onActivated: win.moveShelf(Qt.Key_Right) }
    Shortcut { sequences: ["Return", "Enter"]; enabled: win.shelfKeys && win.shelf.length > 0; onActivated: win.openShelf() }
    Shortcut { sequence: "Delete"; enabled: win.shelfKeys && win.shelf.length > 0; onActivated: backend.forgetPath(win.shelfBook().path) }
    Shortcut {
        sequence: "M"
        enabled: win.shelfKeys && win.shelf.length > 0
        onActivated: {
            const book = win.shelfBook()
            backend.setFinished(book.path, !book.finished)
            win.say(book.finished ? qsTr("Not finished") : qsTr("Finished"))
        }
    }
    Shortcut { sequence: "S"; enabled: win.shelfKeys; onActivated: win.nextSort() }
    Shortcut { sequence: "V"; enabled: win.shelfKeys; onActivated: backend.libraryView = backend.libraryView === "grid" ? "list" : "grid" }
    Shortcut { sequence: "Tab"; enabled: win.shelfKeys; onActivated: win.nextFilter(1) }
    Shortcut { sequences: ["Backtab", "Shift+Backtab", "Shift+Tab"]; enabled: win.shelfKeys; onActivated: win.nextFilter(-1) }

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

            // The library: every book read, and the ones waiting in ~/Books, as
            // a grid of covers or a list, in the order and with the books chosen.
            Item {
                objectName: "library"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.topMargin: 40
                    anchors.bottomMargin: 16
                    anchors.leftMargin: Math.max(24, (parent.width - 1080) / 2)
                    anchors.rightMargin: anchors.leftMargin
                    spacing: 14
                    visible: backend.library.length > 0

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        Label {
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("Library")
                            font.pixelSize: win.uiSize * 2
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        TextField {
                            id: shelfSearch
                            objectName: "shelfSearch"
                            Layout.preferredWidth: 240
                            placeholderText: qsTr("Search titles and authors")
                            Material.roundedScale: theme.radius
                            Keys.onEscapePressed: { text = ""; library.forceActiveFocus() }
                            Keys.onDownPressed: library.forceActiveFocus()
                            Keys.onReturnPressed: { library.forceActiveFocus(); win.openShelf() }
                        }
                    }
                    // Which books, in which order, and how they're shown.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [{ key: "all", label: qsTr("All") }, { key: "reading", label: qsTr("Reading") },
                                    { key: "unread", label: qsTr("Not started") }, { key: "finished", label: qsTr("Finished") }]
                            delegate: Rectangle {
                                id: chip
                                required property var modelData
                                readonly property bool chosen: backend.libraryFilter === modelData.key
                                implicitWidth: chipLabel.implicitWidth + 24
                                implicitHeight: Math.round(win.uiSize * 2.1)
                                radius: Math.min(4, theme.radius)
                                color: chosen ? theme.accent : "transparent"
                                border.color: chosen ? "transparent" : theme.colors.selection
                                Label {
                                    id: chipLabel
                                    anchors.centerIn: parent
                                    text: chip.modelData.label + "  " + win.number(win.countIn(chip.modelData.key))
                                    color: chip.chosen ? theme.accentForeground : theme.foreground
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.libraryFilter = chip.modelData.key
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            objectName: "shelfOrder"
                            color: win.muted
                            text: qsTr("S: %1").arg(win.sortName(backend.librarySort)) + "  ·  "
                                  + (backend.libraryView === "grid" ? qsTr("V: list") : qsTr("V: covers")) + "  ·  " + qsTr("? for keys")
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: win.nextSort()
                            }
                        }
                    }

                    // The books shown: a grid of covers, or a list.
                    FocusScope {
                        id: library
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        GridView {
                            id: shelfGrid
                            objectName: "shelf"
                            anchors.fill: parent
                            visible: backend.libraryView === "grid"
                            clip: true
                            readonly property int columns: Math.max(2, Math.floor(width / 190))
                            cellWidth: Math.floor(width / columns)
                            cellHeight: Math.round(cellWidth * 1.5) + Math.round(win.uiSize * 4.4)
                            model: win.shelf
                            currentIndex: 0
                            highlightMoveDuration: 0
                            keyNavigationEnabled: false
                            ScrollBar.vertical: ScrollBar {}

                            delegate: Item {
                                id: card
                                required property int index
                                required property var modelData
                                width: GridView.view.cellWidth
                                height: GridView.view.cellHeight
                                readonly property bool current: GridView.isCurrentItem

                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    radius: Math.min(6, theme.radius)
                                    color: card.current ? theme.colors.selection : "transparent"
                                }
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 6
                                    BookCover {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.round(width * 1.5)
                                        book: card.modelData
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignLeft
                                        text: card.modelData.title
                                        font.bold: true
                                        elide: Text.ElideRight
                                        opacity: card.modelData.exists ? 1 : 0.5
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignLeft
                                        text: card.modelData.exists ? (card.modelData.author || " ") : qsTr("Missing")
                                        color: win.muted
                                        font.pixelSize: Math.round(win.uiSize * 6 / 7)
                                        elide: Text.ElideRight
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: shelfGrid.currentIndex = card.index
                                    onDoubleClicked: backend.openFile(card.modelData.path)
                                }
                            }
                        }

                        ListView {
                            id: shelfList
                            objectName: "shelfList"
                            anchors.fill: parent
                            visible: backend.libraryView === "list"
                            clip: true
                            spacing: 2
                            model: win.shelf
                            currentIndex: 0
                            highlightMoveDuration: 0
                            keyNavigationEnabled: false
                            ScrollBar.vertical: ScrollBar {}

                            delegate: Rectangle {
                                id: row
                                required property int index
                                required property var modelData
                                width: ListView.view.width
                                height: Math.round(win.uiSize * 5)
                                radius: Math.min(4, theme.radius)
                                color: ListView.isCurrentItem ? theme.colors.selection : "transparent"

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 16
                                    spacing: 14
                                    BookCover {
                                        Layout.preferredHeight: row.height - 12
                                        Layout.preferredWidth: Math.round((row.height - 12) / 1.5)
                                        book: row.modelData
                                        small: true
                                    }
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
                                        text: row.modelData.finished ? qsTr("Finished") : !row.modelData.started ? qsTr("New")
                                              : win.percent(row.modelData.progress)
                                        color: win.muted
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: shelfList.currentIndex = row.index
                                    onDoubleClicked: backend.openFile(row.modelData.path)
                                }
                            }
                        }

                        // A search or a filter with nothing in it.
                        Label {
                            anchors.centerIn: parent
                            width: Math.min(parent.width - 48, 420)
                            visible: win.shelf.length === 0
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            color: win.muted
                            text: shelfSearch.text.trim() !== "" ? qsTr("No books match “%1”.").arg(shelfSearch.text.trim())
                                                                 : qsTr("No books here yet.")
                        }
                    }
                }

                // Nothing read yet: say how to start, never a blank page.
                ColumnLayout {
                    objectName: "empty"
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 480)
                    spacing: 16
                    visible: backend.library.length === 0

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
                        text: qsTr("Open a PDF, EPUB, comic, Markdown or text file, or drop one on the window. Books put in ~/Books appear here by themselves.")
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
                                x: win.turnKind === "slide" ? win.turnSide * stage.width * (1 - win.turnProgress)
                                 : win.turnKind === "cover" && win.turnDirection > 0 ? win.turnSide * stage.width * (1 - win.turnProgress) : 0
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

                            // Ctrl with the wheel zooms a page around the pointer, or sets a
                            // flowing book's text larger or smaller; a pinch zooms too.
                            WheelHandler {
                                acceptedModifiers: Qt.ControlModifier
                                target: null
                                onWheel: event => {
                                    if (backend.paged)
                                        win.zoomAt(Math.pow(1.0015, event.angleDelta.y), point.position.x, point.position.y)
                                    else if (event.angleDelta.y !== 0)
                                        win.resize(event.angleDelta.y > 0 ? 1 : -1)
                                }
                            }
                            PinchHandler {
                                target: null
                                enabled: backend.paged
                                property real last: 1
                                onActiveChanged: last = 1
                                onActiveScaleChanged: {
                                    win.zoomAt(activeScale / last, centroid.position.x, centroid.position.y)
                                    last = activeScale
                                }
                            }
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
                                // A manga's spread starts on the right.
                                layoutDirection: win.bookRightToLeft ? Qt.RightToLeft : Qt.LeftToRight
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
                                        crop: win.crop(modelData)
                                        highlight: theme.accent
                                        spotlight: win.panelView && win.panels.length > 0 && modelData === backend.section
                                                   ? Qt.rect(win.panels[win.panelIndex].x, win.panels[win.panelIndex].y,
                                                             win.panels[win.panelIndex].width, win.panels[win.panelIndex].height)
                                                   : Qt.rect(0, 0, 0, 0)
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
                                    win.paintHighlights()
                                }
                                // The end of a selection made with the mouse, or a click on a highlight.
                                PointHandler {
                                    acceptedButtons: Qt.LeftButton
                                    onActiveChanged: {
                                        if (active)
                                            return
                                        if (prose.selectedText.trim() !== "" && !win.chosenLink && !win.finding) {
                                            win.showSelectionBar(-1)
                                        } else if (prose.selectedText === "" && prose.hoveredLink === "") {
                                            const at = backend.highlightAt(prose.positionAt(point.position.x, point.position.y))
                                            if (at >= 0)
                                                win.showSelectionBar(at)
                                        }
                                    }
                                }
                                onParagraphGapChanged: typeset()
                                Connections {
                                    target: backend
                                    function onSettingsChanged() { prose.typeset() }
                                    function onHighlightsChanged() { win.paintHighlights() }
                                }
                                Connections {
                                    target: theme
                                    function onChanged() { win.paintHighlights() }
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

                        // A ribbon on a bookmarked page, as on a book's corner.
                        Canvas {
                            objectName: "bookmarkRibbon"
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.rightMargin: 28
                            width: 14
                            height: 26
                            visible: win.bookmarkHere
                            onPaint: {
                                const g = getContext("2d")
                                g.reset()
                                g.fillStyle = theme.accent
                                g.beginPath()
                                g.moveTo(0, 0)
                                g.lineTo(width, 0)
                                g.lineTo(width, height)
                                g.lineTo(width / 2, height - 6)
                                g.lineTo(0, height)
                                g.closePath()
                                g.fill()
                            }
                            Connections {
                                target: theme
                                function onChanged() { parent.requestPaint() }
                            }
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
                         : win.turnKind === "slide" ? -win.turnSide * width * win.turnProgress
                         : win.turnKind === "cover" && win.turnDirection < 0 ? -win.turnSide * width * win.turnProgress : 0
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
                        // The whole page, at the size the page shows it, as the page asks for it.
                        sourceSize: Qt.size(Math.round(spreadRow.pageHeight / win.crop(modelData).height * win.fullAspect(modelData)
                                                       * Screen.devicePixelRatio),
                                            Math.round(spreadRow.pageHeight / win.crop(modelData).height * Screen.devicePixelRatio))
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
                    // Out of the way of a panel over the bottom of the page.
                    visible: !settings.visible && !note.visible && !skim.visible
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

    // What can be done with words chosen with the mouse, or a highlight
    // clicked: four colors, a note, copying, looking up, and taking a
    // highlight away. It sits just above the words.
    Popup {
        id: selectionBar
        objectName: "selectionBar"
        property int highlight: -1
        property int start: 0
        property int end: 0
        property real anchorX: 0
        property real anchorY: 0
        x: Math.round(Math.max(8, Math.min(win.width - width - 8, anchorX - width / 2)))
        y: Math.round(anchorY - height - 10 < 8 ? anchorY + win.readingSize * 2 : anchorY - height - 10)
        modal: false
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 8
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }
        onClosed: if (!noteEditor.visible) prose.deselect()

        contentItem: RowLayout {
            spacing: 6
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            Repeater {
                model: ["yellow", "green", "blue", "pink"]
                delegate: Rectangle {
                    id: swatch
                    required property string modelData
                    readonly property bool chosen: selectionBar.highlight >= 0 && selectionBar.highlight < backend.highlights.length
                                                   && backend.highlights[selectionBar.highlight].color === modelData
                    implicitWidth: 26
                    implicitHeight: 26
                    radius: 13
                    color: win.highlightPaint[modelData]
                    border.width: chosen ? 3 : 1
                    border.color: chosen ? win.highlightInk(modelData) : Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.3)
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { win.highlightSelection(swatch.modelData); selectionBar.close() }
                    }
                }
            }
            Rectangle { implicitWidth: 1; implicitHeight: 24; color: theme.colors.selection }
            Button {
                objectName: "noteButton"
                text: qsTr("Note")
                flat: true
                Material.roundedScale: theme.radius
                onClicked: {
                    const index = selectionBar.highlight >= 0 ? selectionBar.highlight : win.highlightSelection(win.lastHighlightColor)
                    selectionBar.close()
                    noteEditor.edit(index)
                }
            }
            Button {
                text: qsTr("Copy")
                flat: true
                Material.roundedScale: theme.radius
                onClicked: {
                    backend.copyText(selectionBar.highlight >= 0 ? backend.highlights[selectionBar.highlight].text : prose.selectedText)
                    win.say(qsTr("Copied"))
                    selectionBar.close()
                }
            }
            Button {
                text: qsTr("Look up")
                flat: true
                Material.roundedScale: theme.radius
                onClicked: win.lookUpSelection()
            }
            Button {
                visible: selectionBar.highlight >= 0
                text: qsTr("Remove")
                flat: true
                Material.roundedScale: theme.radius
                onClicked: { backend.removeHighlight(selectionBar.highlight); selectionBar.close() }
            }
        }
    }

    // A note on a highlight: Ctrl+Enter keeps it, Escape leaves it as it was.
    Popup {
        id: noteEditor
        objectName: "noteEditor"
        property int index: -1
        function edit(at) {
            index = at
            noteInput.text = at >= 0 && at < backend.highlights.length ? backend.highlights[at].note : ""
            open()
        }
        function keep() {
            backend.setHighlightNote(index, noteInput.text)
            close()
        }
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, win.width - 64)
        modal: true
        focus: true
        padding: 20
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }
        onAboutToShow: noteInput.forceActiveFocus()

        contentItem: ColumnLayout {
            spacing: 12
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                text: noteEditor.index >= 0 && noteEditor.index < backend.highlights.length
                      ? backend.highlights[noteEditor.index].text : ""
                font.family: prose.font.family
                font.italic: true
                color: win.muted
                wrapMode: Text.Wrap
                maximumLineCount: 4
                elide: Text.ElideRight
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(win.uiSize * 9)
                TextArea {
                    id: noteInput
                    objectName: "noteInput"
                    placeholderText: qsTr("Your note")
                    wrapMode: TextEdit.Wrap
                    Material.roundedScale: theme.radius
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) {
                            noteEditor.keep()
                            event.accepted = true
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Ctrl+Enter to keep  ·  Esc to cancel")
                    color: win.muted
                    font.pixelSize: Math.round(win.uiSize * 6 / 7)
                }
                Button { text: qsTr("Cancel"); flat: true; Material.roundedScale: theme.radius; onClicked: noteEditor.close() }
                Button { text: qsTr("Keep"); flat: true; Material.roundedScale: theme.radius; onClicked: noteEditor.keep() }
            }
        }
    }

    // A word looked up in a dictionary installed here; with none, it says so
    // and offers to look online, which happens only when chosen.
    Popup {
        id: lookup
        objectName: "lookup"
        property string word: ""
        property string definition: ""
        property bool waiting: false
        function ask(text) {
            word = text
            definition = ""
            waiting = backend.canLookUp()
            open()
            if (waiting)
                backend.lookUp(text)
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
        Connections {
            target: backend
            function onLookedUp(word, definition) {
                if (word === lookup.word) {
                    lookup.definition = definition
                    lookup.waiting = false
                }
            }
        }

        contentItem: ColumnLayout {
            spacing: 10
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                text: lookup.word
                font.family: prose.font.family
                font.pixelSize: Math.round(win.readingSize * 1.1)
                font.bold: true
                elide: Text.ElideRight
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(definitionText.implicitHeight, Math.round(win.height * 0.35))
                visible: lookup.definition !== ""
                Label {
                    id: definitionText
                    width: lookup.availableWidth
                    text: lookup.definition
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                visible: lookup.definition === ""
                color: win.muted
                wrapMode: Text.Wrap
                text: lookup.waiting ? qsTr("Looking up…")
                    : backend.canLookUp() ? qsTr("The dictionary has nothing for this.")
                    : qsTr("No dictionary is installed. sdcv with a StarDict dictionary, or dictd, gives definitions here.")
            }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Esc to close")
                    color: win.muted
                    font.pixelSize: Math.round(win.uiSize * 6 / 7)
                }
                Button {
                    text: qsTr("Look up online")
                    flat: true
                    Material.roundedScale: theme.radius
                    onClicked: {
                        Qt.openUrlExternally("https://en.wiktionary.org/wiki/" + encodeURIComponent(lookup.word.toLowerCase()))
                        lookup.close()
                    }
                }
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

    // The table of contents and the bookmarks, a tab each: Up and Down to
    // choose, Enter to go there, Tab to switch, Delete to remove a bookmark.
    Popup {
        id: contents
        objectName: "contents"
        // Which list: 0 the contents, 1 the bookmarks, 2 the highlights and notes.
        property int tab: 0
        function nextTab() { tab = (tab + 1) % 3 }
        anchors.centerIn: Overlay.overlay
        width: Math.min(600, win.width - 64)
        height: Math.min(contentsColumn.implicitHeight + topPadding + bottomPadding, win.height - 96)
        modal: true
        focus: true
        padding: 20
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }
        // The list takes the keys as the panel starts to appear, not once its
        // opening animation ends, or a key pressed right after T is lost.
        onAboutToShow: {
            tab = 0
            focusList()
        }
        onTabChanged: if (visible) focusList()

        function focusList() {
            if (tab === 1) {
                markList.currentIndex = 0
                markList.forceActiveFocus()
            } else if (tab === 2) {
                noteList.currentIndex = 0
                noteList.forceActiveFocus()
            } else {
                chapters.currentIndex = Math.max(0, backend.tocIndex)
                chapters.positionViewAtIndex(chapters.currentIndex, ListView.Center)
                chapters.forceActiveFocus()
            }
        }
        function choose(index) {
            backend.goToEntry(index)
            win.hintBack()
            Qt.callLater(win.restore)
            close()
        }
        function chooseNote(index) {
            backend.goToHighlight(index)
            win.hintBack()
            close()
        }
        function chooseMark(index) {
            backend.goToBookmark(index)
            win.hintBack()
            Qt.callLater(win.restore)
            close()
        }
        // The first page of a section, as the footer counts them, or "" while
        // they're being counted.
        function pageOf(section) {
            if (backend.paged)
                return win.number(section + 1)
            return backend.pageCount > 0 ? win.number(backend.pageAt(section, 0)) : ""
        }
        // Whether a bookmark lies in the chapter that starts at entry index.
        function marked(index) {
            // A row can outlive its book's contents for a moment as another opens.
            if (index < 0 || index >= backend.toc.length)
                return false
            const from = backend.toc[index].section
            const to = index + 1 < backend.toc.length ? backend.toc[index + 1].section : backend.sectionCount
            return backend.bookmarks.some(b => b.section >= from && (b.section < to || (to === from && b.section === from)))
        }

        ColumnLayout {
            id: contentsColumn
            anchors.fill: parent
            spacing: 12
            // A popup doesn't inherit the window's mirroring.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            RowLayout {
                Layout.fillWidth: true
                spacing: 20
                Repeater {
                    model: [qsTr("Contents"),
                            backend.bookmarks.length > 0 ? qsTr("Bookmarks (%1)").arg(win.number(backend.bookmarks.length)) : qsTr("Bookmarks"),
                            backend.highlights.length > 0 ? qsTr("Notes (%1)").arg(win.number(backend.highlights.length)) : qsTr("Notes")]
                    delegate: Label {
                        required property int index
                        required property string modelData
                        text: modelData
                        font.pixelSize: Math.round(win.uiSize * 10 / 7)
                        font.bold: true
                        color: index === contents.tab ? theme.foreground : win.muted
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: contents.tab = parent.index
                        }
                    }
                }
                // How far through the book, and how long is left, shortened to
                // fit beside the tabs rather than widening the panel.
                Label {
                    objectName: "contentsProgress"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 0
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                    color: win.muted
                    text: backend.minutesLeftInBook > 0
                          ? qsTr("%1 read").arg(win.percent(backend.progress)) + "  ·  " + win.timeLeft(backend.minutesLeftInBook, true)
                          : qsTr("%1 read").arg(win.percent(backend.progress))
                }
            }
            Label {
                Layout.fillWidth: true
                visible: contents.tab === 0 && chapters.count === 0
                text: qsTr("This book has no table of contents. Use [ and ] to move between sections.")
                color: win.muted
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: contents.tab === 1 && markList.count === 0
                text: qsTr("No bookmarks yet. B marks the page you're on.")
                color: win.muted
                wrapMode: Text.Wrap
            }
            ListView {
                id: chapters
                objectName: "chapters"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: contentHeight
                visible: contents.tab === 0 && count > 0
                clip: true
                model: backend.toc
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar {}
                Keys.onReturnPressed: contents.choose(currentIndex)
                Keys.onEnterPressed: contents.choose(currentIndex)
                Keys.onTabPressed: contents.nextTab()

                delegate: Rectangle {
                    id: chapter
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    height: Math.round(win.uiSize * 2.6)
                    radius: Math.min(4, theme.radius)
                    color: ListView.isCurrentItem ? theme.colors.selection : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12 + 18 * Math.min(chapter.modelData.depth, 4)
                        anchors.rightMargin: 12
                        spacing: 10
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            text: chapter.modelData.title
                            font.bold: chapter.index === backend.tocIndex
                            color: chapter.index === backend.tocIndex ? theme.accent : theme.foreground
                            elide: Text.ElideRight
                        }
                        // A bookmark somewhere in this chapter.
                        Rectangle {
                            visible: contents.marked(chapter.index)
                            implicitWidth: 6
                            implicitHeight: 6
                            radius: 3
                            color: theme.accent
                        }
                        Label {
                            text: contents.pageOf(chapter.modelData.section)
                            color: win.muted
                            font.features: { "tnum": 1 }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: contents.choose(chapter.index)
                    }
                }
            }
            ListView {
                id: markList
                objectName: "bookmarks"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: contentHeight
                visible: contents.tab === 1 && count > 0
                clip: true
                model: backend.bookmarks
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar {}
                Keys.onReturnPressed: if (currentIndex >= 0) contents.chooseMark(currentIndex)
                Keys.onEnterPressed: if (currentIndex >= 0) contents.chooseMark(currentIndex)
                Keys.onDeletePressed: if (currentIndex >= 0) backend.removeBookmark(currentIndex)
                Keys.onTabPressed: contents.nextTab()

                delegate: Rectangle {
                    id: mark
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    height: Math.round(win.uiSize * 3.8)
                    radius: Math.min(4, theme.radius)
                    color: ListView.isCurrentItem ? theme.colors.selection : "transparent"

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            text: backend.paged ? qsTr("Page %1").arg(win.number(mark.modelData.section + 1))
                                                : mark.modelData.chapter
                            color: win.muted
                            font.pixelSize: Math.round(win.uiSize * 6 / 7)
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            text: mark.modelData.text
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: contents.chooseMark(mark.index)
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: contents.tab === 2 && noteList.count === 0
                text: qsTr("No highlights yet. Select words with the mouse to highlight them or write a note.")
                color: win.muted
                wrapMode: Text.Wrap
            }
            ListView {
                id: noteList
                objectName: "notebook"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: contentHeight
                visible: contents.tab === 2 && count > 0
                clip: true
                spacing: 4
                model: backend.highlights
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar {}
                Keys.onReturnPressed: if (currentIndex >= 0) contents.chooseNote(currentIndex)
                Keys.onEnterPressed: if (currentIndex >= 0) contents.chooseNote(currentIndex)
                Keys.onDeletePressed: if (currentIndex >= 0) backend.removeHighlight(currentIndex)
                Keys.onTabPressed: contents.nextTab()
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_C && !(event.modifiers & Qt.ControlModifier)) {
                        backend.copyNotes()
                        win.say(qsTr("Copied as Markdown"))
                        event.accepted = true
                    }
                }

                delegate: Rectangle {
                    id: noteRow
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    height: highlightColumn.implicitHeight + 16
                    radius: Math.min(4, theme.radius)
                    color: ListView.isCurrentItem ? theme.colors.selection : "transparent"

                    // The highlight's color, down its side.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.margins: 8
                        width: 3
                        color: win.highlightInk(noteRow.modelData.color)
                    }
                    ColumnLayout {
                        id: highlightColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 22
                        anchors.rightMargin: 12
                        spacing: 3
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            text: noteRow.modelData.chapter
                            color: win.muted
                            font.pixelSize: Math.round(win.uiSize * 6 / 7)
                            elide: Text.ElideRight
                            visible: text !== ""
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            text: noteRow.modelData.text
                            font.family: prose.font.family
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            visible: noteRow.modelData.note !== ""
                            text: noteRow.modelData.note
                            color: win.muted
                            font.italic: true
                            wrapMode: Text.Wrap
                            maximumLineCount: 4
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: contents.chooseNote(noteRow.index)
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                color: win.muted
                font.pixelSize: Math.round(win.uiSize * 6 / 7)
                text: contents.tab === 2 ? qsTr("Enter to go there  ·  Delete to remove  ·  C to copy them all as Markdown")
                    : contents.tab === 1 ? qsTr("Enter to go there  ·  Delete to remove  ·  Tab for notes")
                    : qsTr("Enter to go there  ·  Tab for bookmarks")
            }
        }
    }

    // Skimming the book: where every page and chapter lies, and a page to look
    // at without leaving the one being read. Left and Right move a page,
    // Shift with them a chapter, a typed number goes to that page, Enter goes
    // there and Escape stays. While a book's pages are still being counted,
    // it moves by sections.
    Popup {
        id: skim
        objectName: "skim"
        property int page: 1
        readonly property bool counted: !backend.paged && backend.pageCount > 0
        readonly property int pages: counted ? backend.pageCount : backend.sectionCount
        readonly property int here: {
            void view.contentY
            return hereNow()
        }
        // Worked out on the spot: in a handler of the count's change, the
        // binding above may not have caught up yet.
        function hereNow() {
            return !backend.paged && backend.pageCount > 0 ? backend.pageAt(backend.section, view.contentY) : backend.section + 1
        }
        readonly property var preview: {
            void backend.pageCount
            return visible ? backend.pagePreview(page) : ({})
        }
        // Opened while the pages were being counted, it moved by sections;
        // once they're counted, it looks at the page being read.
        Connections {
            target: backend
            function onPagesChanged() { if (skim.visible) skim.page = skim.hereNow() }
        }
        // The pages chapters start on, for Shift with an arrow and the track's marks.
        readonly property var chapterStarts: {
            const starts = backend.toc.map(e => counted ? backend.pageAt(e.section, 0) : e.section + 1)
            return [...new Set(starts)].sort((a, b) => a - b)
        }
        x: Math.round((win.width - width) / 2)
        y: win.height - height - 8
        width: Math.min(720, win.width - 32)
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
        onAboutToShow: {
            page = hereNow()
            pageNumber.text = ""
            pageNumber.forceActiveFocus()
        }

        function move(to) { page = Math.max(1, Math.min(pages, to)) }
        function chapter(step) {
            const starts = step > 0 ? chapterStarts.filter(p => p > page) : chapterStarts.filter(p => p < page)
            if (starts.length > 0)
                move(step > 0 ? starts[0] : starts[starts.length - 1])
        }
        function go() {
            close()
            if (page === here)
                return
            backend.goToPage(page)
            win.hintBack()
            Qt.callLater(win.restore)
        }

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 12
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    text: skim.preview.chapter || backend.title
                    font.bold: true
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "skimPage"
                    color: win.muted
                    text: skim.counted || backend.paged ? qsTr("Page %1 of %2").arg(win.number(skim.page)).arg(win.number(skim.pages))
                                                        : qsTr("Counting pages…")
                }
            }
            // The page's first words, or, for a PDF or comic, the page itself.
            Label {
                Layout.fillWidth: true
                visible: !backend.paged
                horizontalAlignment: Text.AlignLeft
                text: skim.preview.text || ""
                wrapMode: Text.Wrap
                maximumLineCount: 4
                elide: Text.ElideRight
                font.family: prose.font.family
                font.pixelSize: Math.round(win.readingSize * 0.85)
                lineHeight: 1.3
            }
            Image {
                Layout.alignment: Qt.AlignHCenter
                visible: backend.paged
                Layout.preferredHeight: Math.round(win.height * 0.3)
                Layout.preferredWidth: Math.round(Layout.preferredHeight * win.fullAspect(skim.page - 1))
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                source: backend.paged && skim.visible ? "image://omaread/page/" + backend.bookId + "/" + (skim.page - 1) + win.tint : ""
                sourceSize.height: Math.round(win.height * 0.3 * Screen.devicePixelRatio)
            }
            // The whole book: chapters marked, the page being read, and the
            // page looked at. Books keep their reading direction.
            Item {
                id: track
                objectName: "skimTrack"
                Layout.fillWidth: true
                implicitHeight: 24
                LayoutMirroring.enabled: false
                function xOf(page) { return skim.pages > 1 ? (page - 1) / (skim.pages - 1) * width : 0 }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 2
                    color: theme.colors.selection
                }
                Repeater {
                    model: skim.chapterStarts
                    delegate: Rectangle {
                        required property int modelData
                        x: Math.round(track.xOf(modelData))
                        anchors.verticalCenter: parent.verticalCenter
                        width: 1
                        height: 10
                        color: win.muted
                    }
                }
                Rectangle {
                    x: Math.round(track.xOf(skim.here)) - width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8
                    height: 8
                    radius: 4
                    color: win.muted
                }
                Rectangle {
                    x: Math.round(track.xOf(skim.page)) - width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    radius: 7
                    color: theme.accent
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    function at(x) { skim.move(Math.round(Math.max(0, Math.min(1, x / width)) * (skim.pages - 1)) + 1) }
                    onPressed: mouse => at(mouse.x)
                    onPositionChanged: mouse => at(mouse.x)
                    onReleased: skim.go()
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                TextField {
                    id: pageNumber
                    objectName: "pageField"
                    implicitWidth: 180
                    placeholderText: qsTr("Go to page")
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 1; top: Math.max(1, skim.pages) }
                    Material.roundedScale: theme.radius
                    onTextEdited: if (text !== "") skim.move(parseInt(text))
                    Keys.onReturnPressed: skim.go()
                    Keys.onEnterPressed: skim.go()
                    Keys.onPressed: event => {
                        const chapter = event.modifiers & Qt.ShiftModifier
                        // Mirrored, Left is later in the book, as it is read.
                        const forward = (event.key === Qt.Key_Right) !== win.rightToLeft
                        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                            chapter ? skim.chapter(forward ? 1 : -1) : skim.move(skim.page + (forward ? 1 : -1))
                            event.accepted = true
                        } else if (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp) {
                            skim.chapter(event.key === Qt.Key_PageDown ? 1 : -1)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                            skim.move(event.key === Qt.Key_Home ? 1 : skim.pages)
                            event.accepted = true
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignRight
                    color: win.muted
                    font.pixelSize: Math.round(win.uiSize * 6 / 7)
                    wrapMode: Text.Wrap
                    text: qsTr("← → a page  ·  Shift+← → a chapter  ·  Enter to go  ·  Esc to stay")
                }
            }
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
                    { keys: "G", action: qsTr("Skim the book, or go to a page") },
                    { keys: "T", action: qsTr("Contents, bookmarks, and notes") },
                    { keys: "/  Ctrl+F", action: qsTr("Find in the book") },
                    { keys: "N  Shift+N", action: qsTr("Next or previous match") },
                    { keys: "Ctrl+C", action: qsTr("Copy the selected words") },
                    { keys: "+  −  0", action: qsTr("Larger, smaller, or reset text and pages") },
                    { keys: "V", action: qsTr("Read a comic panel by panel, or whole pages") },
                    { keys: "C", action: qsTr("Trim the white margins of a PDF's or comic's pages") },
                    { keys: "Ctrl+scroll  pinch", action: qsTr("Zoom a page around the pointer") },
                    { keys: "R", action: qsTr("Read a comic or PDF right to left, as manga, or left to right") },
                    { keys: "W", action: qsTr("Fit pages to the width or the window") },
                    { keys: "D", action: qsTr("One page or two side by side") },
                    { keys: "I", action: qsTr("Color pages like the theme") },
                    { keys: "V  S  Tab  /", action: qsTr("Library: covers or a list, the order, which books, search") },
                    { keys: "M", action: qsTr("Library: mark a book finished, or not") },
                    { keys: "B", action: qsTr("Bookmark the page, or remove its bookmark") },
                    { keys: "H", action: qsTr("Highlight the words chosen; click one for a note or a color") },
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
