import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    width: 1380
    height: 870
    minimumWidth: 360
    minimumHeight: 640
    visible: true
    title: "AlternativeFastDelivery"
    color: "#151b1d"
    property bool mobile: width < 820
    property int section: 0
    property int readerTab: 0
    property int fileTab: 0
    property int page: 0
    property bool reading: false
    property var pendingDownloadAction: null
    function requireDownloadFolder(action) {
        if (workstation.needsDownloadFolder) {
            pendingDownloadAction = action;
            downloadSetup.open();
        } else {
            action();
        }
    }
    function chooseFolder() {
        downloadFolderPicker.currentFolder = workstation.folderPickerStart;
        downloadFolderPicker.open();
    }
    function goBack() {
        if (root.section === 3 && root.reading)
            root.reading = false;
        else if (workstation.torrentOpen && root.section !== 1)
            root.section = 1;
        else if (root.section === 1 && workstation.directory.length > 0 && !workstation.busy)
            workstation.openDirectory("..");
        else
            root.section = 0;
    }
    Component.onCompleted: Qt.callLater(function () {
        if (workstation.needsDownloadFolder)
            downloadSetup.open();
    })
    readonly property color ink: "#e3e3d9"
    readonly property color muted: "#8e9b9b"
    readonly property color copper: "#dd935f"
    readonly property color line: "#354042"
    font.family: Qt.platform.os === "windows" ? "Segoe UI" : "sans-serif"
    palette.text: ink
    palette.windowText: ink
    palette.window: "#202a2c"
    palette.base: "#1b2325"
    palette.button: "#2b3233"
    palette.highlight: copper
    palette.highlightedText: "#151b1d"
    Shortcut {
        sequences: [StandardKey.Open]
        enabled: !workstation.busy
        onActivated: {
            localPicker.emailOnly = false;
            localPicker.open();
        }
    }
    Shortcut {
        sequences: [StandardKey.Find]
        enabled: workstation.sourceKind === "mbox"
        onActivated: {
            root.section = 3;
            root.reading = false;
            searchField.forceActiveFocus();
        }
    }
    Shortcut {
        sequences: [StandardKey.Back]
        enabled: !downloadSetup.visible && !localPicker.visible && !torrentPicker.visible && !downloadFolderPicker.visible
        onActivated: root.goBack()
    }
    component Caption: Text {
        color: root.muted
        font.pixelSize: 11
        font.letterSpacing: 1.5
        textFormat: Text.PlainText
    }
    component LabelText: Text {
        color: root.ink
        font.pixelSize: 13
        textFormat: Text.PlainText
    }
    component Field: TextField {
        color: root.ink
        font.pixelSize: 13
        selectByMouse: true
        leftPadding: 12
        implicitHeight: 42
        placeholderTextColor: "#7e8b8d"
        background: Rectangle {
            color: "#111819"
            radius: 3
            border.color: parent.activeFocus ? root.copper : root.line
        }
    }
    component Divider: Rectangle {
        color: root.line
        implicitHeight: 1
        Layout.fillWidth: true
    }
    component Reader: ScrollView {
        property alias content: area.text
        clip: true
        TextArea {
            id: area
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            color: root.ink
            font.family: "monospace"
            font.pixelSize: 13
            wrapMode: TextEdit.Wrap
            padding: root.mobile ? 14 : 24
            background: Rectangle {
                color: "#192123"
            }
        }
    }
    FileDialog {
        id: localPicker
        objectName: "localPicker"
        property bool emailOnly: false
        title: "Open a file"
        nameFilters: emailOnly ? ["Email messages (*.eml)", "All files (*)"] : ["Archives and messages (*.mbox *.eml *.sql)", "All files (*)"]
        onAccepted: {
            workstation.openLocal(selectedFile, emailOnly);
        }
    }
    FileDialog {
        id: torrentPicker
        title: "Open a torrent file"
        nameFilters: ["Torrent files (*.torrent)"]
        onAccepted: {
            const file = selectedFile.toString();
            root.requireDownloadFolder(function () {
                workstation.addTorrent(file);
            });
        }
    }
    FolderDialog {
        id: downloadFolderPicker
        objectName: "downloadFolderPicker"
        title: "Choose a download folder"
        onAccepted: workstation.chooseDownloadFolder(selectedFolder)
    }
    Connections {
        target: workstation
        function onSourceOpened(kind) {
            root.page = 0;
            root.readerTab = 0;
            root.fileTab = 0;
            root.reading = kind === "email";
            root.section = kind === "file" ? 2 : 3;
        }
        function onTorrentOpened() {
            root.section = 1;
        }
        function onMessagesLoaded() {
            root.reading = false;
        }
        function onChanged() {
            if (!workstation.needsDownloadFolder) {
                if (downloadSetup.visible)
                    downloadSetup.close();
                if (!workstation.busy && root.pendingDownloadAction) {
                    const action = root.pendingDownloadAction;
                    root.pendingDownloadAction = null;
                    Qt.callLater(action);
                }
            }
            if (!workstation.busy && root.section === 3 && workstation.messages.length === 0)
                root.reading = false;
        }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            color: "#1d2628"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 14
                Image {
                    source: "qrc:/qt/qml/AFD/resources/afd.svg"
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                }
                Column {
                    LabelText {
                        text: "AFD"
                        font.pixelSize: 20
                        font.weight: Font.Bold
                        font.letterSpacing: 3
                    }
                    Caption {
                        text: "ALTERNATIVE FAST DELIVERY"
                        font.pixelSize: 9
                        font.letterSpacing: 1.4
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                Caption {
                    visible: !root.mobile
                    text: "FILES & EMAIL"
                }
                Rectangle {
                    width: 7
                    height: 7
                    radius: 4
                    color: workstation.busy ? root.copper : "#90ad97"
                    SequentialAnimation on opacity {
                        running: workstation.busy
                        loops: Animation.Infinite
                        NumberAnimation {
                            to: 0.3
                            duration: 650
                        }
                        NumberAnimation {
                            to: 1
                            duration: 650
                        }
                    }
                }
                LabelText {
                    text: workstation.busy ? "Working" : "Ready"
                    color: root.muted
                }
            }
        }
        Divider {}
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            Rectangle {
                visible: !root.mobile
                Layout.preferredWidth: 184
                Layout.fillHeight: true
                color: "#182022"
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 7
                    Caption {
                        text: "WORKSPACE"
                        Layout.topMargin: 16
                        Layout.bottomMargin: 16
                        Layout.leftMargin: 12
                    }
                    Repeater {
                        model: ["Home", "Torrents", "Preview", "Emails", "Signatures", "Settings"]
                        delegate: ItemDelegate {
                            required property int index
                            required property string modelData
                            objectName: "desktopNav" + index
                            Layout.fillWidth: true
                            implicitHeight: 45
                            text: modelData
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            leftPadding: 14
                            rightPadding: 10
                            onClicked: root.section = index
                            background: Rectangle {
                                radius: 3
                                color: root.section === index ? "#303b3c" : parent.hovered ? "#222d2f" : "transparent"
                                border.color: parent.activeFocus ? root.copper : "transparent"
                                Rectangle {
                                    width: 3
                                    height: 18
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: root.copper
                                    visible: root.section === index
                                }
                            }
                            contentItem: Row {
                                spacing: 15
                                Caption {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "0" + (index + 1)
                                    color: root.section === index ? root.copper : "#617477"
                                }
                                LabelText {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData
                                    font.weight: root.section === index ? Font.DemiBold : Font.Normal
                                }
                            }
                        }
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                    Divider {}
                    LabelText {
                        text: "Read files on your device."
                        color: root.muted
                        font.pixelSize: 11
                        Layout.leftMargin: 8
                        Layout.topMargin: 8
                    }
                    Caption {
                        text: "AFD  /  " + Qt.application.version
                        font.pixelSize: 9
                        Layout.leftMargin: 8
                        Layout.bottomMargin: 8
                    }
                }
            }
            Rectangle {
                visible: !root.mobile
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: root.line
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0
                Flickable {
                    visible: root.mobile
                    Layout.fillWidth: true
                    Layout.preferredHeight: 62
                    contentWidth: mobileNav.width + 24
                    clip: true
                    Row {
                        id: mobileNav
                        x: 12
                        y: 6
                        spacing: 6
                        Repeater {
                            model: ["Home", "Torrents", "Preview", "Emails", "Signatures", "Settings"]
                            delegate: AfdButton {
                                required property int index
                                required property string modelData
                                objectName: "mobileNav" + index
                                text: modelData
                                compact: true
                                accent: root.section === index
                                onClicked: root.section = index
                            }
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.mobile ? 80 : 78
                    color: "#151b1d"
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: root.mobile ? 14 : 24
                        spacing: 14
                        Column {
                            Layout.fillWidth: true
                            spacing: 5
                            Caption {
                                text: ["HOME", "TORRENTS", "FILE PREVIEW", "EMAILS", "SIGNATURES", "SETTINGS"][root.section]
                            }
                            LabelText {
                                text: workstation.sourceName
                                font.pixelSize: root.mobile ? 15 : 20
                                font.weight: Font.DemiBold
                                width: parent.width
                                elide: Text.ElideMiddle
                            }
                        }
                        AfdButton {
                            objectName: "openFileButton"
                            text: "Open file"
                            compact: root.mobile
                            onClicked: {
                                localPicker.emailOnly = false;
                                localPicker.open();
                            }
                            enabled: !workstation.busy
                        }
                    }
                }
                Divider {}
                RowLayout {
                    visible: workstation.torrentOpen && root.section !== 1
                    Layout.fillWidth: true
                    Layout.leftMargin: 14
                    Layout.rightMargin: 14
                    Layout.topMargin: visible ? 6 : 0
                    Layout.bottomMargin: visible ? 6 : 0
                    AfdButton {
                        objectName: "backToFilesButton"
                        text: "← Torrent files"
                        compact: true
                        onClicked: root.section = 1
                    }
                    LabelText {
                        text: workstation.directory
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        visible: !root.mobile
                        color: root.muted
                    }
                }
                StackLayout {
                    currentIndex: root.section
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // Overview
                    ScrollView {
                        clip: true
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: parent.width
                            spacing: 20
                            Item {
                                height: 5
                            }
                            Caption {
                                text: "OPEN SOMETHING"
                                Layout.leftMargin: 24
                            }
                            GridLayout {
                                columns: root.mobile ? 1 : 2
                                columnSpacing: 16
                                rowSpacing: 16
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 196
                                    color: "#202a2c"
                                    border.color: root.line
                                    radius: 3
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 22
                                        spacing: 12
                                        Caption {
                                            text: "LOCAL FILES"
                                            color: root.copper
                                        }
                                        LabelText {
                                            text: "Read your files."
                                            font.pixelSize: 19
                                            font.weight: Font.DemiBold
                                        }
                                        LabelText {
                                            text: "Open email archives, messages or other files on this device."
                                            wrapMode: Text.Wrap
                                            Layout.fillWidth: true
                                            color: root.muted
                                        }
                                        Item {
                                            Layout.fillHeight: true
                                        }
                                        AfdButton {
                                            objectName: "chooseLocalButton"
                                            text: "Open a file"
                                            accent: true
                                            onClicked: {
                                                localPicker.emailOnly = false;
                                                localPicker.open();
                                            }
                                            enabled: !workstation.busy
                                        }
                                    }
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 196
                                    color: "#202a2c"
                                    border.color: root.line
                                    radius: 3
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 22
                                        spacing: 12
                                        Caption {
                                            text: "TORRENTS"
                                            color: "#a4b9ae"
                                        }
                                        LabelText {
                                            text: "Browse a torrent."
                                            font.pixelSize: 19
                                            font.weight: Font.DemiBold
                                        }
                                        LabelText {
                                            text: "Download the parts you want to read."
                                            wrapMode: Text.Wrap
                                            Layout.fillWidth: true
                                            color: root.muted
                                        }
                                        Item {
                                            Layout.fillHeight: true
                                        }
                                        AfdButton {
                                            objectName: "openTorrentWorkspaceButton"
                                            text: "Open torrents"
                                            onClicked: root.section = 1
                                        }
                                    }
                                }
                            }
                            Caption {
                                text: "STATUS"
                                Layout.leftMargin: 24
                                Layout.topMargin: 10
                            }
                            LabelText {
                                text: workstation.coverage
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }
                            LabelText {
                                text: workstation.stats
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                color: root.muted
                            }
                            Divider {
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                            }
                            LabelText {
                                text: "Made with <3 by ApfelTeeSaft!"
                                color: root.muted
                                font.pixelSize: 12
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                            }
                            Item {
                                height: 16
                            }
                        }
                    }
                    // Torrents
                    ColumnLayout {
                        spacing: 14
                        LabelText {
                            text: "Other peers can see your IP address. BitTorrent may upload downloaded data."
                            visible: !workstation.torrentOpen
                            wrapMode: Text.Wrap
                            color: root.muted
                            Layout.fillWidth: true
                            Layout.margins: 20
                        }
                        RowLayout {
                            visible: !workstation.torrentOpen
                            Layout.fillWidth: true
                            Layout.leftMargin: 20
                            Layout.rightMargin: 20
                            Field {
                                id: magnet
                                objectName: "magnetField"
                                Layout.fillWidth: true
                                placeholderText: "magnet:?xt=…"
                                onAccepted: {
                                    if (!workstation.busy && !workstation.torrentOpen && text.trim().length > 0) {
                                        const link = text;
                                        root.requireDownloadFolder(function () {
                                            workstation.addTorrent(link);
                                        });
                                    }
                                }
                            }
                            AfdButton {
                                objectName: "addTorrentButton"
                                text: "Add"
                                accent: true
                                onClicked: {
                                    const link = magnet.text;
                                    root.requireDownloadFolder(function () {
                                        workstation.addTorrent(link);
                                    });
                                }
                                enabled: !workstation.busy && !workstation.torrentOpen && magnet.text.trim().length > 0
                            }
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.leftMargin: 20
                            Layout.rightMargin: 20
                            Layout.preferredHeight: childrenRect.height
                            spacing: 8
                            AfdButton {
                                text: "Open .torrent"
                                visible: !workstation.torrentOpen
                                onClicked: torrentPicker.open()
                                enabled: !workstation.busy && !workstation.torrentOpen
                            }
                            AfdButton {
                                text: "Reopen torrent"
                                visible: !workstation.torrentOpen
                                onClicked: root.requireDownloadFolder(function () {
                                    workstation.restoreTorrent();
                                })
                                enabled: !workstation.busy && !workstation.torrentOpen
                            }
                            AfdButton {
                                text: "Pause"
                                visible: workstation.torrentOpen
                                onClicked: workstation.pauseTorrent(true)
                                enabled: workstation.torrentOpen
                            }
                            AfdButton {
                                text: "Resume"
                                visible: workstation.torrentOpen
                                onClicked: workstation.pauseTorrent(false)
                                enabled: !workstation.busy && workstation.torrentOpen
                            }
                            AfdButton {
                                objectName: "closeTorrentButton"
                                text: "Close torrent"
                                visible: workstation.torrentOpen
                                onClicked: workstation.closeTorrent()
                                enabled: !workstation.busy && workstation.torrentOpen
                            }
                        }
                        Caption {
                            text: "TORRENT FILES"
                            Layout.leftMargin: 20
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 20
                            Layout.rightMargin: 20
                            AfdButton {
                                objectName: "parentFolderButton"
                                text: "↑ Up"
                                compact: true
                                enabled: !workstation.busy && workstation.directory.length > 0
                                onClicked: workstation.openDirectory("..")
                            }
                            Caption {
                                text: workstation.directory || "All files"
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                font.letterSpacing: 0
                            }
                        }
                        ListView {
                            objectName: "torrentFilesList"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: workstation.files
                            reuseItems: true
                            delegate: Rectangle {
                                required property var modelData
                                objectName: modelData.folder ? "torrentFolder:" + modelData.path : "torrentFile:" + modelData.id
                                width: ListView.view.width
                                height: 62
                                color: torrentMouse.containsMouse ? "#2b3638" : "#1d2628"
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 18
                                    LabelText {
                                        text: (modelData.folder ? "▸  " : "") + modelData.label
                                        Layout.fillWidth: true
                                        elide: Text.ElideMiddle
                                    }
                                    Caption {
                                        text: Number(modelData.size).toLocaleString() + " B"
                                    }
                                    LabelText {
                                        text: "↗"
                                        color: root.copper
                                    }
                                }
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: 1
                                    color: root.line
                                }
                                MouseArea {
                                    id: torrentMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: !workstation.busy
                                    onClicked: {
                                        if (modelData.folder)
                                            workstation.openDirectory(modelData.path);
                                        else {
                                            workstation.selectFile(modelData.id);
                                            root.section = 2;
                                        }
                                    }
                                }
                            }
                            LabelText {
                                anchors.centerIn: parent
                                visible: workstation.files.length === 0
                                text: "Add a torrent to see its files."
                                color: root.muted
                            }
                            ScrollBar.vertical: ScrollBar {}
                        }
                    }
                    // Files
                    ColumnLayout {
                        spacing: 12
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.margins: 14
                            ColumnLayout {
                                Layout.fillWidth: true
                                Caption {
                                    text: "START BYTE"
                                    font.letterSpacing: 0
                                }
                                Field {
                                    id: offset
                                    objectName: "offsetField"
                                    text: "0"
                                    Layout.fillWidth: true
                                    Accessible.name: "Start byte"
                                    inputMethodHints: Qt.ImhDigitsOnly
                                }
                            }
                            ColumnLayout {
                                Layout.preferredWidth: root.mobile ? 80 : 120
                                Caption {
                                    text: "BYTES"
                                    font.letterSpacing: 0
                                }
                                Field {
                                    id: length
                                    objectName: "lengthField"
                                    text: "4096"
                                    Layout.fillWidth: true
                                    Accessible.name: "Number of bytes"
                                    inputMethodHints: Qt.ImhDigitsOnly
                                }
                            }
                            AfdButton {
                                objectName: "readRangeButton"
                                text: "Read"
                                accent: true
                                onClicked: workstation.inspectRange(offset.text, length.text)
                                enabled: !workstation.busy && workstation.sourceKind !== "none"
                            }
                        }
                        RowLayout {
                            Layout.leftMargin: 14
                            Layout.rightMargin: 14
                            AfdButton {
                                text: "Hex"
                                compact: true
                                accent: root.fileTab === 0
                                onClicked: root.fileTab = 0
                            }
                            AfdButton {
                                objectName: "rangeTextButton"
                                text: "Text"
                                compact: true
                                accent: root.fileTab === 1
                                onClicked: root.fileTab = 1
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Caption {
                                text: "64 KiB MAX"
                                visible: !root.mobile
                            }
                        }
                        Reader {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            content: root.fileTab === 0 ? workstation.hex : workstation.fileText
                        }
                    }
                    // Emails
                    ColumnLayout {
                        spacing: 0
                        Flow {
                            visible: (!root.mobile || !root.reading) && workstation.sourceKind !== "email"
                            Layout.fillWidth: true
                            Layout.margins: 12
                            Layout.preferredHeight: childrenRect.height
                            spacing: 8
                            AfdButton {
                                objectName: "quickSampleButton"
                                text: "Sample"
                                compact: true
                                enabled: !workstation.busy && workstation.sourceKind === "mbox"
                                onClicked: workstation.quickInspect()
                            }
                            AfdButton {
                                objectName: "indexArchiveButton"
                                text: "Scan emails"
                                accent: true
                                compact: true
                                enabled: !workstation.busy && workstation.sourceKind === "mbox"
                                onClicked: workstation.indexArchive()
                            }
                            AfdButton {
                                text: "Show all"
                                compact: true
                                enabled: workstation.canBrowse && workstation.sourceKind === "mbox"
                                onClicked: {
                                    root.page = 0;
                                    searchField.clear();
                                    workstation.search("", 0);
                                }
                            }
                            AfdButton {
                                text: "Stop"
                                compact: true
                                enabled: workstation.busy
                                onClicked: workstation.cancel()
                            }
                        }
                        LabelText {
                            text: workstation.sourceKind === "none" ? "Open an email archive or message. Scan an archive to search all its emails." : workstation.coverage
                            visible: !root.mobile || !root.reading
                            font.pixelSize: 11
                            color: root.muted
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                            Layout.leftMargin: 14
                            Layout.rightMargin: 14
                            Layout.bottomMargin: 12
                        }
                        RowLayout {
                            visible: (!root.mobile || !root.reading) && workstation.sourceKind !== "email"
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.bottomMargin: 12
                            Field {
                                id: searchField
                                objectName: "searchField"
                                Layout.fillWidth: true
                                placeholderText: "Search scanned emails…"
                                onAccepted: {
                                    root.page = 0;
                                    if (workstation.sourceKind === "mbox")
                                        workstation.search(text, 0);
                                }
                            }
                            AfdButton {
                                objectName: "findButton"
                                text: "Find"
                                enabled: workstation.canBrowse && workstation.sourceKind === "mbox"
                                onClicked: {
                                    root.page = 0;
                                    workstation.search(searchField.text, 0);
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 1
                            ColumnLayout {
                                visible: !root.mobile || !root.reading
                                Layout.preferredWidth: root.mobile ? root.width : 380
                                Layout.fillWidth: root.mobile
                                Layout.fillHeight: true
                                spacing: 0
                                ListView {
                                    objectName: "messageList"
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    model: workstation.messages
                                    clip: true
                                    reuseItems: true
                                    delegate: Rectangle {
                                        required property var modelData
                                        objectName: "messageRow" + modelData.id
                                        width: ListView.view.width
                                        height: 112
                                        color: mailMouse.containsMouse ? "#2e3a3c" : "#20292b"
                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 14
                                            spacing: 5
                                            LabelText {
                                                text: modelData.sender
                                                font.weight: Font.DemiBold
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                            LabelText {
                                                text: modelData.subject || "(no subject)"
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                            Caption {
                                                text: "TO  " + modelData.recipient
                                                font.letterSpacing: 0
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Caption {
                                                    text: modelData.date
                                                    font.pixelSize: 9
                                                    font.letterSpacing: 0
                                                    Layout.fillWidth: true
                                                    elide: Text.ElideRight
                                                }
                                                Caption {
                                                    text: (modelData.attachment ? "◇  " : "") + modelData.size + " B"
                                                    font.letterSpacing: 0
                                                }
                                            }
                                            Caption {
                                                text: modelData.dkim
                                                color: modelData.dkim === "Valid" ? "#a1c5ac" : root.copper
                                                font.letterSpacing: 0
                                                font.pixelSize: 10
                                            }
                                        }
                                        Rectangle {
                                            anchors.bottom: parent.bottom
                                            width: parent.width
                                            height: 1
                                            color: root.line
                                        }
                                        MouseArea {
                                            id: mailMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            enabled: workstation.canBrowse
                                            onClicked: {
                                                workstation.inspectMessage(modelData.id);
                                                root.reading = true;
                                            }
                                        }
                                    }
                                    LabelText {
                                        visible: workstation.messages.length === 0
                                        anchors.centerIn: parent
                                        text: workstation.sourceKind === "none" ? "Open a file to browse messages" : "No emails found"
                                        font.pixelSize: 11
                                        color: root.muted
                                    }
                                    ScrollBar.vertical: ScrollBar {}
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.margins: 8
                                    AfdButton {
                                        text: "←"
                                        compact: true
                                        enabled: root.page > 0 && workstation.canBrowse
                                        onClicked: {
                                            --root.page;
                                            workstation.search(searchField.text, root.page);
                                        }
                                    }
                                    Caption {
                                        text: "PAGE " + (root.page + 1)
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                    AfdButton {
                                        text: "→"
                                        compact: true
                                        enabled: workstation.messages.length === 100 && workstation.canBrowse
                                        onClicked: {
                                            ++root.page;
                                            workstation.search(searchField.text, root.page);
                                        }
                                    }
                                }
                            }
                            ColumnLayout {
                                visible: !root.mobile || root.reading
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 0
                                Flickable {
                                    objectName: "readerTabsViewport"
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: root.mobile ? 64 : 50
                                    contentWidth: readerTabs.width + 16
                                    clip: true
                                    Row {
                                        id: readerTabs
                                        x: 8
                                        y: 8
                                        spacing: 6
                                        AfdButton {
                                            objectName: "backToListButton"
                                            text: "← Emails"
                                            compact: true
                                            visible: root.mobile
                                            onClicked: root.reading = false
                                        }
                                        Repeater {
                                            model: ["Body", "Source", "Headers", "Parts"]
                                            delegate: AfdButton {
                                                required property int index
                                                required property string modelData
                                                objectName: "readerTab" + index
                                                text: modelData
                                                compact: true
                                                accent: root.readerTab === index
                                                onClicked: root.readerTab = index
                                            }
                                        }
                                    }
                                }
                                Reader {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    content: workstation.raw.length === 0 ? "Choose an email to read it." : [workstation.text, workstation.raw, workstation.headers, workstation.mime][Math.min(root.readerTab, 3)]
                                }
                            }
                        }
                    }
                    // DKIM
                    ColumnLayout {
                        spacing: 16
                        LabelText {
                            text: "Open an email to check its signatures. Online checks look up its signing domain using DNS."
                            wrapMode: Text.Wrap
                            color: root.muted
                            Layout.fillWidth: true
                            Layout.margins: 20
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.leftMargin: 20
                            Layout.rightMargin: 20
                            Layout.preferredHeight: childrenRect.height
                            spacing: 8
                            AfdButton {
                                text: "Open email"
                                onClicked: {
                                    localPicker.emailOnly = true;
                                    localPicker.open();
                                }
                                enabled: !workstation.busy
                            }
                            AfdButton {
                                text: "Check online"
                                accent: true
                                onClicked: workstation.verify(true)
                                enabled: !workstation.busy && workstation.raw.length > 0
                            }
                            AfdButton {
                                text: "Check offline"
                                onClicked: workstation.verify(false)
                                enabled: !workstation.busy && workstation.raw.length > 0
                            }
                        }
                        Reader {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            content: workstation.signatures || "Open an email first. A valid signature confirms its signing domain, not who wrote it."
                        }
                    }
                    // Settings
                    ScrollView {
                        clip: true
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: parent.width
                            spacing: 18
                            Caption {
                                text: "DOWNLOAD FOLDER"
                                Layout.leftMargin: 24
                                Layout.topMargin: 20
                            }
                            LabelText {
                                text: workstation.downloadFolder || "No folder chosen"
                                wrapMode: Text.WrapAnywhere
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                            }
                            AfdButton {
                                objectName: "changeDownloadFolderButton"
                                text: "Change folder…"
                                Layout.leftMargin: 24
                                enabled: !workstation.busy
                                onClicked: root.chooseFolder()
                            }
                            LabelText {
                                text: "New torrents use this folder. Existing downloads stay where they are."
                                color: root.muted
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                            }
                            LabelText {
                                visible: workstation.torrentOpen
                                text: "Current torrent: " + workstation.torrentFolder
                                color: root.muted
                                wrapMode: Text.WrapAnywhere
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                            }
                            LabelText {
                                visible: workstation.downloadFolderError.length > 0
                                text: workstation.downloadFolderError
                                color: root.copper
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                            }
                            Caption {
                                text: "UPLOADS"
                                Layout.leftMargin: 24
                                Layout.topMargin: 24
                            }
                            LabelText {
                                text: "Upload speed limit (KiB/s). Pause the torrent to stop transfers."
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                color: root.muted
                            }
                            RowLayout {
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Field {
                                    id: upload
                                    Component.onCompleted: text = String(workstation.uploadKiB)
                                    Layout.preferredWidth: 100
                                    validator: IntValidator {
                                        bottom: 1
                                        top: 1048576
                                    }
                                }
                                AfdButton {
                                    text: "Apply"
                                    onClicked: workstation.uploadLimit(Number(upload.text))
                                    enabled: !workstation.busy && upload.acceptableInput
                                }
                            }
                            Divider {
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                            }
                            Caption {
                                text: "DOWNLOAD LIMIT"
                                Layout.leftMargin: 24
                            }
                            LabelText {
                                text: "Limit how much downloaded data this torrent can keep (MiB). A full scan may need a larger limit."
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                color: root.muted
                            }
                            RowLayout {
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Field {
                                    id: quota
                                    Component.onCompleted: text = String(workstation.quotaMiB)
                                    Layout.preferredWidth: 100
                                    validator: IntValidator {
                                        bottom: 16
                                        top: 1048576
                                    }
                                }
                                AfdButton {
                                    text: "Save limit"
                                    onClicked: workstation.storageLimit(Number(quota.text))
                                    enabled: !workstation.busy && quota.acceptableInput
                                }
                            }
                            AfdButton {
                                text: "Delete torrent downloads…"
                                Layout.leftMargin: 24
                                onClicked: cleanup.open()
                                enabled: !workstation.busy && workstation.torrentOpen
                            }
                            AfdButton {
                                text: "Check saved search data"
                                Layout.leftMargin: 24
                                onClicked: workstation.checkIndex()
                                enabled: !workstation.busy
                            }
                            AfdButton {
                                text: "Reset search data…"
                                Layout.leftMargin: 24
                                onClicked: rebuild.open()
                                enabled: !workstation.busy
                            }
                            Divider {
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                            }
                            LabelText {
                                text: "AFD 0.1.0"
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                color: root.muted
                                font.pixelSize: 12
                            }
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.mobile ? 72 : 46
                    color: "#202a2c"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        LabelText {
                            text: workstation.notice
                            color: root.copper
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        AfdButton {
                            text: "Stop"
                            compact: true
                            visible: workstation.busy
                            onClicked: workstation.cancel()
                        }
                    }
                }
            }
        }
        Rectangle {
            visible: !root.mobile
            Layout.fillWidth: true
            Layout.preferredHeight: 29
            color: "#101718"
            Caption {
                anchors.verticalCenter: parent.verticalCenter
                x: 20
                text: workstation.stats
                font.letterSpacing: 0
                font.pixelSize: 10
            }
        }
    }
    Dialog {
        id: downloadSetup
        objectName: "downloadSetup"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(480, root.width - 32)
        title: "Where should downloads go?"
        modal: true
        closePolicy: Popup.NoAutoClose
        padding: 20
        header: Label {
            text: downloadSetup.title
            color: root.ink
            font.pixelSize: 18
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
            padding: 20
            bottomPadding: 4
        }
        contentItem: ColumnLayout {
            spacing: 16
            LabelText {
                text: "Choose a folder for torrent downloads. You can change it in Settings."
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            LabelText {
                text: workstation.suggestedDownloadPath
                wrapMode: Text.WrapAnywhere
                color: root.muted
                Layout.fillWidth: true
            }
            AfdButton {
                objectName: "useSuggestedFolderButton"
                text: "Use this folder"
                accent: true
                enabled: !workstation.busy
                onClicked: workstation.chooseDownloadFolder(workstation.suggestedDownloadFolder)
            }
            AfdButton {
                objectName: "chooseDownloadFolderButton"
                text: "Choose another folder…"
                enabled: !workstation.busy
                onClicked: root.chooseFolder()
            }
            LabelText {
                text: workstation.downloadFolderError
                visible: text.length > 0
                color: root.copper
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            AfdButton {
                objectName: "downloadSetupLaterButton"
                text: "Later"
                enabled: !workstation.busy
                onClicked: {
                    root.pendingDownloadAction = null;
                    downloadSetup.close();
                }
            }
        }
    }
    Dialog {
        id: rebuild
        title: "Reset search data?"
        anchors.centerIn: parent
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        LabelText {
            text: "Your files stay untouched. A backup of the old search data is kept. Scan the file again to rebuild search."
            wrapMode: Text.Wrap
            width: Math.min(320, root.width - 64)
        }
        onAccepted: workstation.resetIndex()
    }
    Dialog {
        id: cleanup
        title: "Delete downloaded files?"
        anchors.centerIn: parent
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        LabelText {
            text: "This removes this torrent’s downloaded files. To keep them, choose Close torrent instead."
            wrapMode: Text.Wrap
            width: Math.min(320, root.width - 64)
        }
        onAccepted: workstation.clearTorrent()
    }
}
