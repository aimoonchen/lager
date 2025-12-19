// main.qml
// Editor Qt UI - QML Interface
//
// Layout:
// +------------------+----------------------+
// |   Object Tree    |   Property Panel     |
// |   (Left Panel)   |   (Right Panel)      |
// |                  |                      |
// +------------------+----------------------+
// |            Status Bar / Toolbar         |
// +-----------------------------------------+

import QtQuick 2.12
import QtQuick.Controls 2.12
import QtQuick.Controls.Material 2.12
import QtQuick.Layouts 1.12

import Lager.Example.Editor 1.0 as Editor

ApplicationWindow {
    id: window
    width: 1200
    height: 800
    visible: true
    title: qsTr("Lager Editor - Scene Editor")

    Material.theme: Material.Dark
    Material.accent: Material.Blue

    Editor.EditorApp {
        id: editorApp
    }

    // ============================================================
    // Toolbar
    // ============================================================
    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8

            ToolButton {
                text: qsTr("⟲ Undo")
                enabled: editorApp.canUndo
                onClicked: editorApp.undo()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Undo last action (Ctrl+Z)")
            }

            ToolButton {
                text: qsTr("⟳ Redo")
                enabled: editorApp.canRedo
                onClicked: editorApp.redo()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Redo last undone action (Ctrl+Y)")
            }

            ToolSeparator {}

            Label {
                text: qsTr("History: %1 undo / %2 redo").arg(editorApp.undoStackSize).arg(editorApp.redoStackSize)
                opacity: 0.7
            }

            Item { Layout.fillWidth: true }

            ToolButton {
                text: qsTr("🔄 Sync to Engine")
                onClicked: editorApp.syncToEngine()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Publish changes to engine process")
            }

            Label {
                text: qsTr("Objects: %1").arg(editorApp.objectCount)
                font.bold: true
            }
        }
    }

    // ============================================================
    // Main Content
    // ============================================================
    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        // Left Panel: Object Tree
        Pane {
            SplitView.preferredWidth: 300
            SplitView.minimumWidth: 200

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    text: qsTr("Scene Objects")
                    font.bold: true
                    font.pixelSize: 16
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    ListView {
                        id: objectListView
                        model: editorApp.getObjectList()
                        spacing: 2

                        delegate: ItemDelegate {
                            width: objectListView.width
                            height: 48
                            highlighted: modelData.id === editorApp.selectedObjectId

                            contentItem: RowLayout {
                                spacing: 8

                                // Type icon
                                Label {
                                    text: {
                                        switch (modelData.type) {
                                            case "Transform": return "📦"
                                            case "Light": return "💡"
                                            case "Camera": return "📷"
                                            default: return "📄"
                                        }
                                    }
                                    font.pixelSize: 20
                                }

                                ColumnLayout {
                                    spacing: 2
                                    Layout.fillWidth: true

                                    Label {
                                        text: modelData.name
                                        font.bold: true
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: modelData.type + " [" + modelData.id + "]"
                                        font.pixelSize: 10
                                        opacity: 0.6
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                }
                            }

                            onClicked: {
                                editorApp.selectObject(modelData.id)
                            }
                        }
                    }
                }
            }
        }

        // Right Panel: Property Editor
        Pane {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 400

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                // Header
                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: editorApp.selectedObject 
                              ? qsTr("Properties: %1").arg(editorApp.selectedObject.name)
                              : qsTr("No Object Selected")
                        font.bold: true
                        font.pixelSize: 16
                        Layout.fillWidth: true
                    }

                    Label {
                        visible: editorApp.selectedObject !== null
                        text: editorApp.selectedObject ? editorApp.selectedObject.type : ""
                        opacity: 0.6
                    }
                }

                // Property list
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 4

                        Repeater {
                            model: editorApp.selectedObject ? editorApp.selectedObject.propertyCount : 0

                            delegate: PropertyEditor {
                                Layout.fillWidth: true
                                property var prop: editorApp.selectedObject.property(index)
                            }
                        }

                        // Placeholder when no object selected
                        Item {
                            visible: !editorApp.selectedObject
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Label {
                                anchors.centerIn: parent
                                text: qsTr("Select an object from the left panel to edit its properties")
                                opacity: 0.5
                            }
                        }
                    }
                }
            }
        }
    }

    // ============================================================
    // Status Bar
    // ============================================================
    footer: ToolBar {
        height: 32

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            Label {
                text: editorApp.selectedObjectId 
                      ? qsTr("Selected: %1").arg(editorApp.selectedObjectId)
                      : qsTr("No selection")
                opacity: 0.7
            }

            Item { Layout.fillWidth: true }

            Label {
                text: qsTr("Lager Editor v1.0")
                opacity: 0.5
            }
        }
    }

    // Keyboard shortcuts
    Shortcut {
        sequence: StandardKey.Undo
        onActivated: editorApp.undo()
    }

    Shortcut {
        sequence: StandardKey.Redo
        onActivated: editorApp.redo()
    }
}

// ============================================================
// PropertyEditor Component - Dynamic property editing widget
// ============================================================
component PropertyEditor: Pane {
    id: propertyPane
    required property var prop

    visible: prop !== null
    padding: 8

    background: Rectangle {
        color: propertyPane.prop && propertyPane.prop.category === "Transform" 
               ? Qt.rgba(0.2, 0.3, 0.4, 0.3) 
               : Qt.rgba(0.2, 0.2, 0.2, 0.3)
        radius: 4
    }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        // Property name
        Label {
            text: prop ? prop.displayName : ""
            Layout.preferredWidth: 120
            elide: Text.ElideRight

            ToolTip.visible: propMouseArea.containsMouse && prop && prop.tooltip
            ToolTip.text: prop ? prop.tooltip : ""

            MouseArea {
                id: propMouseArea
                anchors.fill: parent
                hoverEnabled: true
            }
        }

        // Dynamic widget based on type
        Loader {
            Layout.fillWidth: true
            sourceComponent: {
                if (!prop) return null
                switch (prop.widgetType) {
                    case "LineEdit": return lineEditComponent
                    case "SpinBox": return spinBoxComponent
                    case "DoubleSpinBox": return doubleSpinBoxComponent
                    case "CheckBox": return checkBoxComponent
                    case "Slider": return sliderComponent
                    case "ComboBox": return comboBoxComponent
                    case "Vector3Edit": return vector3Component
                    case "ReadOnly": return readOnlyComponent
                    default: return lineEditComponent
                }
            }
        }
    }

    // ============================================================
    // Widget Components
    // ============================================================

    Component {
        id: lineEditComponent
        TextField {
            text: prop ? prop.value : ""
            enabled: prop ? !prop.readOnly : false
            onEditingFinished: {
                if (prop) prop.value = text
            }
        }
    }

    Component {
        id: spinBoxComponent
        SpinBox {
            value: prop ? prop.value : 0
            from: prop ? prop.minValue : 0
            to: prop ? prop.maxValue : 100
            stepSize: prop ? prop.step : 1
            enabled: prop ? !prop.readOnly : false
            onValueModified: {
                if (prop) prop.value = value
            }
        }
    }

    Component {
        id: doubleSpinBoxComponent
        RowLayout {
            SpinBox {
                id: doubleSpinBox
                property real realValue: prop ? prop.value : 0
                property real realFrom: prop ? prop.minValue : 0
                property real realTo: prop ? prop.maxValue : 100
                property real realStep: prop ? prop.step : 0.1
                property int decimals: 2
                property real factor: Math.pow(10, decimals)

                from: realFrom * factor
                to: realTo * factor
                stepSize: realStep * factor
                value: realValue * factor
                enabled: prop ? !prop.readOnly : false

                textFromValue: function(value, locale) {
                    return Number(value / factor).toLocaleString(locale, 'f', decimals)
                }

                valueFromText: function(text, locale) {
                    return Number.fromLocaleString(locale, text) * factor
                }

                onValueModified: {
                    if (prop) prop.value = value / factor
                }
            }
        }
    }

    Component {
        id: checkBoxComponent
        CheckBox {
            checked: prop ? prop.value : false
            enabled: prop ? !prop.readOnly : false
            onToggled: {
                if (prop) prop.value = checked
            }
        }
    }

    Component {
        id: sliderComponent
        RowLayout {
            Slider {
                id: slider
                Layout.fillWidth: true
                from: prop ? prop.minValue : 0
                to: prop ? prop.maxValue : 100
                stepSize: prop ? prop.step : 1
                value: prop ? prop.value : 0
                enabled: prop ? !prop.readOnly : false
                onMoved: {
                    if (prop) prop.value = value
                }
            }
            Label {
                text: slider.value.toFixed(1)
                Layout.preferredWidth: 50
            }
        }
    }

    Component {
        id: comboBoxComponent
        ComboBox {
            model: prop ? prop.options : []
            currentIndex: prop ? prop.options.indexOf(prop.value) : -1
            enabled: prop ? !prop.readOnly : false
            onActivated: {
                if (prop && index >= 0) prop.value = model[index]
            }
        }
    }

    Component {
        id: vector3Component
        RowLayout {
            spacing: 4

            Label { text: "X:" }
            SpinBox {
                id: xSpinBox
                property real factor: 100
                from: -10000
                to: 10000
                value: prop && prop.value && prop.value.x ? prop.value.x * factor : 0
                enabled: prop ? !prop.readOnly : false
                textFromValue: function(v) { return (v / factor).toFixed(2) }
                valueFromText: function(t) { return parseFloat(t) * factor }
                onValueModified: updateVector3()
            }

            Label { text: "Y:" }
            SpinBox {
                id: ySpinBox
                property real factor: 100
                from: -10000
                to: 10000
                value: prop && prop.value && prop.value.y ? prop.value.y * factor : 0
                enabled: prop ? !prop.readOnly : false
                textFromValue: function(v) { return (v / factor).toFixed(2) }
                valueFromText: function(t) { return parseFloat(t) * factor }
                onValueModified: updateVector3()
            }

            Label { text: "Z:" }
            SpinBox {
                id: zSpinBox
                property real factor: 100
                from: -10000
                to: 10000
                value: prop && prop.value && prop.value.z ? prop.value.z * factor : 0
                enabled: prop ? !prop.readOnly : false
                textFromValue: function(v) { return (v / factor).toFixed(2) }
                valueFromText: function(t) { return parseFloat(t) * factor }
                onValueModified: updateVector3()
            }

            function updateVector3() {
                if (prop) {
                    prop.value = {
                        x: xSpinBox.value / xSpinBox.factor,
                        y: ySpinBox.value / ySpinBox.factor,
                        z: zSpinBox.value / zSpinBox.factor
                    }
                }
            }
        }
    }

    Component {
        id: readOnlyComponent
        Label {
            text: prop ? String(prop.value) : ""
            opacity: 0.7
        }
    }
}
