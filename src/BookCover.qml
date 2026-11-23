import QtQuick
import QtQuick.Controls

// A book on the library's shelf: its cover, or one set from its title in the
// theme's colors when it has none, with how far it has been read along the
// bottom, or a mark once it's finished.
Item {
    id: cover

    // A library entry: {title, author, cover, progress, started, finished, exists}.
    property var book: ({})
    // In the list, where only the cover's colors and a letter fit.
    property bool small: false

    readonly property var hues: ["blue", "green", "magenta", "cyan", "red", "yellow"]
    // The same book gets the same color every time.
    readonly property color hue: {
        const title = book.title || ""
        let hash = 0
        for (let i = 0; i < title.length; ++i)
            hash = (hash * 31 + title.charCodeAt(i)) % 1000003
        const name = hues[hash % hues.length]
        return theme.colors[name] || theme.accent
    }

    Rectangle {
        anchors.fill: parent
        radius: Math.min(4, theme.radius)
        color: Qt.tint(theme.background, Qt.rgba(cover.hue.r, cover.hue.g, cover.hue.b, 0.28))
        border.color: Qt.rgba(theme.foreground.r, theme.foreground.g, theme.foreground.b, 0.12)
        clip: true
        opacity: cover.book.exists === false ? 0.5 : 1

        Image {
            id: art
            anchors.fill: parent
            source: cover.book.cover || ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: true
            sourceSize.height: cover.small ? 160 : 480
            visible: status === Image.Ready
        }
        // No cover of its own: the title, set like a book's.
        Column {
            anchors.centerIn: parent
            width: parent.width - (cover.small ? 6 : 24)
            spacing: 10
            visible: art.status !== Image.Ready
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: cover.small ? (cover.book.title || "?").charAt(0) : (cover.book.title || "")
                font.family: "Noto Serif"
                font.pixelSize: cover.small ? Math.round(cover.height * 0.45) : Math.round(cover.width / 9)
                font.bold: true
                wrapMode: Text.Wrap
                maximumLineCount: 5
                elide: Text.ElideRight
                color: theme.foreground
            }
            Rectangle {
                visible: !cover.small && (cover.book.author || "") !== ""
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width * 0.3
                height: 1
                color: cover.hue
            }
            Label {
                visible: !cover.small
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: cover.book.author || ""
                font.family: "Noto Serif"
                font.pixelSize: Math.round(cover.width / 14)
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                color: theme.foreground
                opacity: 0.8
            }
        }

        // How far it's read, along the foot of the cover.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: cover.small ? 3 : 5
            visible: cover.book.started === true && cover.book.finished !== true
            color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.75)
            Rectangle {
                width: parent.width * Math.max(0.02, cover.book.progress || 0)
                height: parent.height
                color: theme.accent
            }
        }
    }
    // Finished: a mark in the corner.
    Rectangle {
        visible: cover.book.finished === true
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: cover.small ? 2 : 8
        width: cover.small ? 14 : 26
        height: width
        radius: width / 2
        color: theme.accent
        Label {
            anchors.centerIn: parent
            text: "✓"
            color: theme.accentForeground
            font.pixelSize: Math.round(parent.width * 0.6)
            font.bold: true
        }
    }
}
