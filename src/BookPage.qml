import QtQuick
import QtQuick.Window

// One page of a paged book: the drawn page, the search matches and links on
// it, and an area dragged across it to copy its words.
Item {
    id: sheet

    required property int page
    // "/<background>/<foreground>" to color the page like the theme, or "".
    required property string tint
    required property color highlight
    // The area dragged across the page, as fractions of it; empty for none.
    property rect selected: Qt.rect(0, 0, 0, 0)
    readonly property bool hasSelection: selected.width > 0 && selected.height > 0
    // In panel view, the panel being read, as fractions of the page: the rest
    // of the page is dimmed around it. Empty for none.
    property rect spotlight: Qt.rect(0, 0, 0, 0)

    // A link on the page: another page of the book, or a web address.
    signal follow(int target, string url)
    // A new drag began here, so other pages let go of theirs.
    signal selectionStarted()

    function clearSelection() { selected = Qt.rect(0, 0, 0, 0) }

    Rectangle {
        anchors.fill: parent
        color: "white"
        visible: image.status !== Image.Ready
        opacity: 0.08
    }

    Image {
        id: image
        objectName: "pageImage"
        anchors.fill: parent
        source: "image://omaread/page/" + backend.bookId + "/" + sheet.page + sheet.tint
        sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
        asynchronous: true
        retainWhileLoading: true
        fillMode: Image.PreserveAspectFit
    }

    // Dragging marks words to copy; a plain click lets go of them.
    MouseArea {
        anchors.fill: parent
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        property point start
        onPressed: mouse => {
            start = Qt.point(mouse.x, mouse.y)
            sheet.clearSelection()
            sheet.selectionStarted()
        }
        onPositionChanged: mouse => {
            const x = Math.max(0, Math.min(start.x, mouse.x)), y = Math.max(0, Math.min(start.y, mouse.y))
            const right = Math.min(width, Math.max(start.x, mouse.x)), bottom = Math.min(height, Math.max(start.y, mouse.y))
            if (right - x > 3 && bottom - y > 3)
                sheet.selected = Qt.rect(x / width, y / height, (right - x) / width, (bottom - y) / height)
        }
    }

    Rectangle {
        visible: sheet.hasSelection
        x: sheet.selected.x * sheet.width
        y: sheet.selected.y * sheet.height
        width: sheet.selected.width * sheet.width
        height: sheet.selected.height * sheet.height
        color: Qt.rgba(sheet.highlight.r, sheet.highlight.g, sheet.highlight.b, 0.18)
        border.color: sheet.highlight
    }

    Repeater {
        model: backend.pageHits.filter(hit => hit.page === sheet.page)
        delegate: Rectangle {
            required property var modelData
            x: modelData.x * sheet.width - 2
            y: modelData.y * sheet.height - 2
            width: modelData.width * sheet.width + 4
            height: modelData.height * sheet.height + 4
            radius: 2
            color: Qt.rgba(sheet.highlight.r, sheet.highlight.g, sheet.highlight.b, modelData.current ? 0.45 : 0.2)
            border.color: modelData.current ? sheet.highlight : "transparent"
        }
    }

    Repeater {
        model: backend.links.filter(link => link.page === sheet.page)
        delegate: MouseArea {
            required property var modelData
            x: modelData.x * sheet.width
            y: modelData.y * sheet.height
            width: modelData.width * sheet.width
            height: modelData.height * sheet.height
            cursorShape: Qt.PointingHandCursor
            onClicked: sheet.follow(modelData.target, modelData.url)
        }
    }

    // Everything but the panel being read, dimmed in the window's color.
    Item {
        id: shade
        anchors.fill: parent
        visible: sheet.spotlight.width > 0
        readonly property real edgeLeft: sheet.spotlight.x * sheet.width
        readonly property real edgeTop: sheet.spotlight.y * sheet.height
        readonly property real edgeRight: (sheet.spotlight.x + sheet.spotlight.width) * sheet.width
        readonly property real edgeBottom: (sheet.spotlight.y + sheet.spotlight.height) * sheet.height
        Repeater {
            model: [Qt.rect(0, 0, 1, 0), Qt.rect(0, 1, 1, 0), Qt.rect(0, 2, 0, 0), Qt.rect(1, 2, 0, 0)]
            delegate: Rectangle {
                required property var modelData
                x: modelData.y === 2 ? (modelData.x === 0 ? 0 : shade.edgeRight) : 0
                y: modelData.y === 0 ? 0 : modelData.y === 1 ? shade.edgeBottom : shade.edgeTop
                width: modelData.y === 2 ? (modelData.x === 0 ? shade.edgeLeft : sheet.width - shade.edgeRight) : sheet.width
                height: modelData.y === 0 ? shade.edgeTop : modelData.y === 1 ? sheet.height - shade.edgeBottom : shade.edgeBottom - shade.edgeTop
                color: theme.background
                opacity: 0.82
            }
        }
    }
}
