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

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property color muted: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.6)
    // Single-key shortcuts rest while a popup has the keyboard.
    readonly property bool busy: help.opened || contents.opened
    readonly property bool reading: backend.open && !busy
    readonly property bool browsing: !backend.open && !busy

    // How a page is drawn: fitted whole into the window, or across its width,
    // and then scaled by zoom.
    property bool fitWidth: false
    property real zoom: 1
    // Set while the view is moved to the saved place, so that move isn't
    // taken for the reader scrolling.
    property bool restoring: false

    function number(n) { return Number(n).toLocaleString(Qt.locale(), 'f', 0) }
    function percent(p) { return qsTr("%1%").arg(number(Math.round(p * 100))) }

    // One screen on, overlapping a little so no line is lost; at the end of a
    // section, the start of the next.
    function forward() {
        if (view.contentY + view.height >= view.contentHeight - 2) {
            if (backend.section < backend.sectionCount - 1)
                jump(backend.section + 1, 0)
        } else {
            view.contentY = Math.min(view.contentY + step(), view.contentHeight - view.height)
        }
    }
    function backward() {
        if (view.contentY <= 1) {
            if (backend.section > 0)
                jump(backend.section - 1, 1)
        } else {
            view.contentY = Math.max(0, view.contentY - step())
        }
    }
    function step() { return Math.max(40, view.height - 3 * backend.textSize) }
    function scroll(dy) {
        view.contentY = Math.max(0, Math.min(view.contentY + dy, view.contentHeight - view.height))
    }
    function jump(section, position) {
        backend.goTo(section, position)
        Qt.callLater(restore)
    }
    function restore() {
        restoring = true
        view.contentY = backend.position * Math.max(0, view.contentHeight - view.height)
        view.contentX = Math.max(0, (view.contentWidth - view.width) / 2)
        restoring = false
    }
    function resize(by) {
        if (backend.paged)
            zoom = by === 0 ? 1 : Math.max(0.5, Math.min(4, zoom * (by > 0 ? 1.2 : 1 / 1.2)))
        else
            backend.textSize = by === 0 ? 20 : backend.textSize + 2 * by
    }
    function leave() {
        if (backend.error !== "")
            backend.dismissError()
        else if (win.visibility === Window.FullScreen)
            win.showNormal()
        else if (backend.open)
            backend.close()
    }

    Connections {
        target: backend
        function onBookChanged() { win.zoom = 1; Qt.callLater(win.restore) }
        function onSectionChanged() { Qt.callLater(win.restore) }
    }

    Shortcut { sequence: "?"; enabled: !contents.opened; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequence: "Q"; enabled: !win.busy; onActivated: Qt.quit() }
    Shortcut { sequence: "O"; enabled: !win.busy; onActivated: backend.pickFile() }
    Shortcut { sequence: "F"; enabled: !win.busy; onActivated: win.visibility === Window.FullScreen ? win.showNormal() : win.showFullScreen() }
    Shortcut { sequence: "Escape"; enabled: !win.busy; onActivated: win.leave() }

    Shortcut { sequences: ["Space", "PgDown", "Right"]; enabled: win.reading; onActivated: win.forward() }
    Shortcut { sequences: ["Shift+Space", "PgUp", "Left"]; enabled: win.reading; onActivated: win.backward() }
    Shortcut { sequences: ["Down", "J"]; enabled: win.reading; onActivated: win.scroll(60) }
    Shortcut { sequences: ["Up", "K"]; enabled: win.reading; onActivated: win.scroll(-60) }
    Shortcut { sequence: "]"; enabled: win.reading; onActivated: win.jump(backend.section + 1, 0) }
    Shortcut { sequence: "["; enabled: win.reading; onActivated: win.jump(backend.section - 1, 0) }
    Shortcut { sequence: "Home"; enabled: win.reading; onActivated: win.jump(0, 0) }
    Shortcut { sequence: "End"; enabled: win.reading; onActivated: win.jump(backend.sectionCount - 1, 0) }
    Shortcut { sequence: "T"; enabled: backend.open && !help.opened; onActivated: contents.opened ? contents.close() : contents.open() }
    Shortcut { sequences: ["+", "="]; enabled: win.reading; onActivated: win.resize(1) }
    Shortcut { sequence: "-"; enabled: win.reading; onActivated: win.resize(-1) }
    Shortcut { sequence: "0"; enabled: win.reading; onActivated: win.resize(0) }
    Shortcut { sequence: "W"; enabled: win.reading && backend.paged; onActivated: { win.fitWidth = !win.fitWidth; win.zoom = 1 } }
    Shortcut { sequence: "I"; enabled: win.reading && backend.paged; onActivated: backend.tintPages = !backend.tintPages }

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
                            text: qsTr("Library")
                            font.pixelSize: 28
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
                            height: 64
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

                                    Label {
                                        Layout.fillWidth: true
                                        text: row.modelData.title
                                        font.pixelSize: 16
                                        font.bold: true
                                        elide: Text.ElideRight
                                        opacity: row.modelData.exists ? 1 : 0.5
                                    }
                                    Label {
                                        Layout.fillWidth: true
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
                        font.pixelSize: 28
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

                Flickable {
                    id: view
                    objectName: "view"
                    anchors.fill: parent
                    anchors.bottomMargin: statusBar.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    contentWidth: backend.paged ? Math.max(width, page.width + 2 * page.margin) : width
                    contentHeight: backend.paged ? page.height + 2 * page.margin : prose.height + 2 * prose.margin
                    ScrollBar.vertical: ScrollBar {}
                    ScrollBar.horizontal: ScrollBar {}

                    onContentYChanged: if (!win.restoring && contentHeight > height)
                        backend.position = contentY / (contentHeight - height)
                    onContentHeightChanged: Qt.callLater(win.restore)
                    onHeightChanged: Qt.callLater(win.restore)

                    // A paged book's page, fitted to the window and then zoomed.
                    Image {
                        id: page
                        objectName: "page"
                        readonly property int margin: 16
                        readonly property real aspect: backend.paged ? (backend.aspects[backend.section] || 0.7071) : 1
                        readonly property real fitted: win.fitWidth
                            ? view.width - 2 * margin
                            : Math.min(view.width - 2 * margin, (view.height - 2 * margin) * aspect)
                        readonly property string tint: backend.tintPages
                            ? "/" + String(theme.background).slice(1, 7) + "/" + String(theme.foreground).slice(1, 7) : ""
                        visible: backend.paged
                        x: Math.max(margin, (view.width - width) / 2)
                        y: margin
                        width: Math.max(48, fitted * win.zoom)
                        height: width / aspect
                        source: backend.paged ? "image://omaread/page/" + backend.bookId + "/" + backend.section + tint : ""
                        sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
                        asynchronous: true
                        retainWhileLoading: true
                        fillMode: Image.PreserveAspectFit
                    }

                    // A flowing book's chapter, in the theme's colors and the
                    // reader's text size.
                    Text {
                        id: prose
                        objectName: "prose"
                        readonly property int margin: 40
                        visible: !backend.paged
                        x: (view.width - width) / 2
                        y: margin
                        width: Math.max(200, Math.min(view.width - 2 * 32, backend.textSize * 34))
                        text: backend.text
                        textFormat: backend.textFormat
                        baseUrl: backend.baseUrl
                        wrapMode: Text.Wrap
                        font.pixelSize: backend.textSize
                        lineHeight: 1.35
                        color: theme.foreground
                        linkColor: theme.accent
                        onLinkActivated: link => { if (!backend.followLink(link)) Qt.openUrlExternally(link) }

                        HoverHandler {
                            cursorShape: prose.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
                        }
                    }
                }

                // The next page, drawn ahead so turning to it is instant.
                Image {
                    visible: false
                    asynchronous: true
                    readonly property int next: backend.section + 1
                    readonly property real aspect: backend.aspects[next] || page.aspect
                    source: backend.paged && next < backend.sectionCount
                        ? "image://omaread/page/" + backend.bookId + "/" + next + page.tint : ""
                    sourceSize: Qt.size(Math.round(page.width * Screen.devicePixelRatio),
                                        Math.round(page.width / aspect * Screen.devicePixelRatio))
                }

                Binding {
                    target: backend
                    property: "readingWidth"
                    value: prose.width
                }

                RowLayout {
                    id: statusBar
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    height: 32
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
                        text: backend.tocIndex >= 0 ? backend.toc[backend.tocIndex].title : ""
                        color: win.muted
                        elide: Text.ElideRight
                    }
                    Label {
                        objectName: "where"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 2
                        horizontalAlignment: Text.AlignRight
                        text: (backend.paged
                               ? qsTr("Page %1 of %2").arg(win.number(backend.section + 1)).arg(win.number(backend.sectionCount))
                               : qsTr("Section %1 of %2").arg(win.number(backend.section + 1)).arg(win.number(backend.sectionCount)))
                              + "  ·  " + win.percent(backend.progress)
                        color: win.muted
                        elide: Text.ElideLeft
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
            anchors.bottomMargin: 48
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
            if (index >= 0 && index < backend.toc.length)
                win.jump(backend.toc[index].section, 0)
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
                font.pixelSize: 20
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
                    height: 36
                    radius: Math.min(4, theme.radius)
                    color: ListView.isCurrentItem ? theme.colors.selection : "transparent"

                    Label {
                        anchors.fill: parent
                        anchors.leftMargin: 12 + 18 * Math.min(chapter.modelData.depth, 4)
                        anchors.rightMargin: 12
                        verticalAlignment: Text.AlignVCenter
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
                    { keys: "T", action: qsTr("Table of contents") },
                    { keys: "+  −  0", action: qsTr("Larger, smaller, or reset text and pages") },
                    { keys: "W", action: qsTr("Fit pages to the width or the window") },
                    { keys: "I", action: qsTr("Color pages like the theme") },
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
                        Layout.preferredWidth: 170
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
