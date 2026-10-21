import QtQuick
import QtQuick.Controls
import QtQuick.Window

Button {
    id: control
    property bool accent: false
    property bool compact: false
    implicitWidth: label.implicitWidth + 28
    implicitHeight: Window.window && Window.window.width < 820 ? 48 : compact ? 32 : 42
    opacity: enabled ? 1 : 0.45
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Keys.onReturnPressed: if (enabled)
        clicked()
    // AbstractButton supplies mouse, touch, keyboard and accessibility activation.
    background: Rectangle {
        color: !control.enabled ? "#252a2b" : control.down ? (control.accent ? "#bd7749" : "#4a5658") : control.hovered ? (control.accent ? "#f0a974" : "#3a4142") : (control.accent ? "#dd935f" : "#2b3233")
        border.color: control.activeFocus || control.accent ? "#dd935f" : "#465051"
        border.width: control.activeFocus ? 2 : 1
        radius: 3
        Behavior on color {
            ColorAnimation {
                duration: 100
            }
        }
    }
    contentItem: Text {
        id: label
        text: control.text
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: control.accent ? "#192020" : "#e3e3d9"
        font.pixelSize: 13
        font.weight: Font.DemiBold
    }
}
