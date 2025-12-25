
// editor_main.cpp
// Editor Process (Process A) - Combined QML/Widgets version
//
// Build options:
// - EDITOR_USE_QML=1 : Use QML interface
// - EDITOR_USE_QML=0 or undefined : Use Qt Widgets interface
// - EDITOR_HAS_QML : Automatically defined by CMake when QML support is available

#include <lager_ext/editor_engine.h>
#include <lager_ext/value.h>

#include <lager/store.hpp>

#include <QApplication>
#include <QMainWindow>
#include <QMessageBox>
#include <iostream>

#if defined(EDITOR_USE_QML) && EDITOR_USE_QML && defined(EDITOR_HAS_QML)
    #define USE_QML_UI 1
#else
    #define USE_QML_UI 0
#endif

#if USE_QML_UI
    #include <lager/event_loop/qml.hpp>
    #include <QQmlApplicationEngine>
    #include <QQmlContext>
    #include <QQuickStyle>
#else
    #include <lager/event_loop/qt.hpp>
#endif

#include <lager/extra/qt.hpp>

// Qt Widgets includes
#include <QDockWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QScrollArea>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QPushButton>
#include <QToolBar>
#include <QAction>
#include <QStatusBar>
#include <QSplitter>
#include <QHeaderView>
#include <QStyle>
#include <QTimer>

#include <memory>
#include <map>

using namespace lager_ext;

// ============================================================
// Helper: Convert Value to QVariant
// ============================================================

QVariant valueToQVariant(const Value& val) {
    return std::visit([](const auto& v) -> QVariant {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return QVariant();
        } else if constexpr (std::is_same_v<T, bool>) {
            return QVariant(v);
        } else if constexpr (std::is_same_v<T, int>) {
            return QVariant(v);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            return QVariant(static_cast<qlonglong>(v));
        } else if constexpr (std::is_same_v<T, float>) {
            return QVariant(static_cast<double>(v));
        } else if constexpr (std::is_same_v<T, double>) {
            return QVariant(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return QVariant(QString::fromStdString(v));
        } else if constexpr (std::is_same_v<T, ValueVector>) {
            QVariantList list;
            for (const auto& item : v) {
                list.append(valueToQVariant(*item));
            }
            return list;
        } else if constexpr (std::is_same_v<T, ValueMap>) {
            QVariantMap map;
            for (const auto& [key, val] : v) {
                map[QString::fromStdString(key)] = valueToQVariant(*val);
            }
            return map;
        } else {
            return QVariant();
        }
    }, val.data);
}

Value qvariantToValue(const QVariant& var) {
    switch (var.typeId()) {
        case QMetaType::UnknownType:
            return Value{};
        case QMetaType::Bool:
            return Value{var.toBool()};
        case QMetaType::Int:
        case QMetaType::LongLong:
            return Value{static_cast<int64_t>(var.toLongLong())};
        case QMetaType::Double:
        case QMetaType::Float:
            return Value{var.toDouble()};
        case QMetaType::QString:
            return Value{var.toString().toStdString()};
        case QMetaType::QVariantList: {
            ValueVector vec;
            for (const auto& item : var.toList()) {
                vec = vec.push_back(immer::box<Value>(qvariantToValue(item)));
            }
            return Value{vec};
        }
        case QMetaType::QVariantMap: {
            ValueMap map;
            const auto qmap = var.toMap();
            for (auto it = qmap.begin(); it != qmap.end(); ++it) {
                map = map.set(it.key().toStdString(),
                              immer::box<Value>(qvariantToValue(it.value())));
            }
            return Value{map};
        }
        default:
            return Value{var.toString().toStdString()};
    }
}

// ============================================================
// PropertyWidget - Creates appropriate widget for property type
// ============================================================

class PropertyWidget : public QWidget
{
    Q_OBJECT

public:
    PropertyWidget(const PropertyMeta& meta, QWidget* parent = nullptr)
        : QWidget(parent)
        , meta_(meta)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        createWidget(layout);
    }

    void setValue(const Value& val) {
        blockSignals(true);
        updateWidgetValue(val);
        blockSignals(false);
    }

    Value getValue() const {
        return getWidgetValue();
    }

signals:
    void valueChanged(const Value& newValue);

private:
    void createWidget(QHBoxLayout* layout) {
        switch (meta_.widget_type) {
            case WidgetType::LineEdit: {
                auto* edit = new QLineEdit(this);
                edit->setReadOnly(meta_.read_only);
                connect(edit, &QLineEdit::editingFinished, this, [this, edit]() {
                    emit valueChanged(Value{edit->text().toStdString()});
                });
                widget_ = edit;
                break;
            }
            case WidgetType::SpinBox: {
                auto* spin = new QSpinBox(this);
                spin->setReadOnly(meta_.read_only);
                if (meta_.range) {
                    spin->setRange(static_cast<int>(meta_.range->min_value),
                                   static_cast<int>(meta_.range->max_value));
                    spin->setSingleStep(static_cast<int>(meta_.range->step));
                }
                connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this](int val) {
                    emit valueChanged(Value{static_cast<int64_t>(val)});
                });
                widget_ = spin;
                break;
            }
            case WidgetType::DoubleSpinBox: {
                auto* spin = new QDoubleSpinBox(this);
                spin->setReadOnly(meta_.read_only);
                spin->setDecimals(3);
                if (meta_.range) {
                    spin->setRange(meta_.range->min_value, meta_.range->max_value);
                    spin->setSingleStep(meta_.range->step);
                }
                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, [this](double val) {
                    emit valueChanged(Value{val});
                });
                widget_ = spin;
                break;
            }
            case WidgetType::CheckBox: {
                auto* check = new QCheckBox(this);
                check->setEnabled(!meta_.read_only);
                connect(check, &QCheckBox::toggled, this, [this](bool checked) {
                    emit valueChanged(Value{checked});
                });
                widget_ = check;
                break;
            }
            case WidgetType::Slider: {
                auto* container = new QWidget(this);
                auto* hbox = new QHBoxLayout(container);
                hbox->setContentsMargins(0, 0, 0, 0);

                auto* slider = new QSlider(Qt::Horizontal, this);
                auto* label = new QLabel(this);
                label->setMinimumWidth(50);

                if (meta_.range) {
                    slider->setRange(static_cast<int>(meta_.range->min_value),
                                     static_cast<int>(meta_.range->max_value));
                }
                slider->setEnabled(!meta_.read_only);

                connect(slider, &QSlider::valueChanged, this, [this, label](int val) {
                    label->setText(QString::number(val));
                    emit valueChanged(Value{static_cast<int64_t>(val)});
                });

                hbox->addWidget(slider, 1);
                hbox->addWidget(label);
                widget_ = container;
                slider_ = slider;
                sliderLabel_ = label;
                break;
            }
            case WidgetType::ComboBox: {
                auto* combo = new QComboBox(this);
                combo->setEnabled(!meta_.read_only);
                if (meta_.combo_options) {
                    for (const auto& opt : meta_.combo_options->options) {
                        combo->addItem(QString::fromStdString(opt));
                    }
                }
                connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this, combo](int) {
                    emit valueChanged(Value{combo->currentText().toStdString()});
                });
                widget_ = combo;
                break;
            }
            case WidgetType::Vector3Edit: {
                auto* container = new QWidget(this);
                auto* hbox = new QHBoxLayout(container);
                hbox->setContentsMargins(0, 0, 0, 0);
                hbox->setSpacing(4);

                auto createSpinBox = [this, hbox](const QString& label) {
                    hbox->addWidget(new QLabel(label, this));
                    auto* spin = new QDoubleSpinBox(this);
                    spin->setDecimals(2);
                    spin->setRange(-10000, 10000);
                    spin->setEnabled(!meta_.read_only);
                    hbox->addWidget(spin, 1);
                    return spin;
                };

                xSpin_ = createSpinBox("X:");
                ySpin_ = createSpinBox("Y:");
                zSpin_ = createSpinBox("Z:");

                auto emitVector = [this]() {
                    ValueMap map;
                    map = map.set("x", immer::box<Value>(Value{xSpin_->value()}));
                    map = map.set("y", immer::box<Value>(Value{ySpin_->value()}));
                    map = map.set("z", immer::box<Value>(Value{zSpin_->value()}));
                    emit valueChanged(Value{map});
                };

                connect(xSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, emitVector);
                connect(ySpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, emitVector);
                connect(zSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, emitVector);

                widget_ = container;
                break;
            }
            case WidgetType::ReadOnly:
            default: {
                auto* label = new QLabel(this);
                label->setStyleSheet("color: gray;");
                widget_ = label;
                break;
            }
        }

        layout->addWidget(widget_);
    }

    void updateWidgetValue(const Value& val) {
        switch (meta_.widget_type) {
            case WidgetType::LineEdit: {
                if (auto* edit = qobject_cast<QLineEdit*>(widget_)) {
                    if (auto* str = val.get_if<std::string>()) {
                        edit->setText(QString::fromStdString(*str));
                    }
                }
                break;
            }
            case WidgetType::SpinBox: {
                if (auto* spin = qobject_cast<QSpinBox*>(widget_)) {
                    if (auto* num = val.get_if<int64_t>()) {
                        spin->setValue(static_cast<int>(*num));
                    } else if (auto* d = val.get_if<double>()) {
                        spin->setValue(static_cast<int>(*d));
                    }
                }
                break;
            }
            case WidgetType::DoubleSpinBox: {
                if (auto* spin = qobject_cast<QDoubleSpinBox*>(widget_)) {
                    if (auto* num = val.get_if<double>()) {
                        spin->setValue(*num);
                    } else if (auto* i = val.get_if<int64_t>()) {
                        spin->setValue(static_cast<double>(*i));
                    }
                }
                break;
            }
            case WidgetType::CheckBox: {
                if (auto* check = qobject_cast<QCheckBox*>(widget_)) {
                    if (auto* b = val.get_if<bool>()) {
                        check->setChecked(*b);
                    }
                }
                break;
            }
            case WidgetType::Slider: {
                if (slider_) {
                    if (auto* num = val.get_if<int64_t>()) {
                        slider_->setValue(static_cast<int>(*num));
                        if (sliderLabel_) sliderLabel_->setText(QString::number(*num));
                    }
                }
                break;
            }
            case WidgetType::ComboBox: {
                if (auto* combo = qobject_cast<QComboBox*>(widget_)) {
                    if (auto* str = val.get_if<std::string>()) {
                        combo->setCurrentText(QString::fromStdString(*str));
                    }
                }
                break;
            }
            case WidgetType::Vector3Edit: {
                if (xSpin_ && ySpin_ && zSpin_) {
                    if (auto* map = val.get_if<ValueMap>()) {
                        if (auto it = map->find("x"); it) {
                            if (auto* d = (*it)->get_if<double>()) xSpin_->setValue(*d);
                        }
                        if (auto it = map->find("y"); it) {
                            if (auto* d = (*it)->get_if<double>()) ySpin_->setValue(*d);
                        }
                        if (auto it = map->find("z"); it) {
                            if (auto* d = (*it)->get_if<double>()) zSpin_->setValue(*d);
                        }
                    }
                }
                break;
            }
            case WidgetType::ReadOnly:
            default: {
                if (auto* label = qobject_cast<QLabel*>(widget_)) {
                    label->setText(QString::fromStdString(value_to_string(val)));
                }
                break;
            }
        }
    }

    Value getWidgetValue() const {
        switch (meta_.widget_type) {
            case WidgetType::LineEdit: {
                if (auto* edit = qobject_cast<QLineEdit*>(widget_)) {
                    return Value{edit->text().toStdString()};
                }
                break;
            }
            case WidgetType::SpinBox: {
                if (auto* spin = qobject_cast<QSpinBox*>(widget_)) {
                    return Value{static_cast<int64_t>(spin->value())};
                }
                break;
            }
            case WidgetType::DoubleSpinBox: {
                if (auto* spin = qobject_cast<QDoubleSpinBox*>(widget_)) {
                    return Value{spin->value()};
                }
                break;
            }
            case WidgetType::CheckBox: {
                if (auto* check = qobject_cast<QCheckBox*>(widget_)) {
                    return Value{check->isChecked()};
                }
                break;
            }
            case WidgetType::Slider: {
                if (slider_) {
                    return Value{static_cast<int64_t>(slider_->value())};
                }
                break;
            }
            case WidgetType::ComboBox: {
                if (auto* combo = qobject_cast<QComboBox*>(widget_)) {
                    return Value{combo->currentText().toStdString()};
                }
                break;
            }
            case WidgetType::Vector3Edit: {
                if (xSpin_ && ySpin_ && zSpin_) {
                    ValueMap map;
                    map = map.set("x", immer::box<Value>(Value{xSpin_->value()}));
                    map = map.set("y", immer::box<Value>(Value{ySpin_->value()}));
                    map = map.set("z", immer::box<Value>(Value{zSpin_->value()}));
                    return Value{map};
                }
                break;
            }
            default:
                break;
        }
        return Value{};
    }

    PropertyMeta meta_;
    QWidget* widget_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* sliderLabel_ = nullptr;
    QDoubleSpinBox* xSpin_ = nullptr;
    QDoubleSpinBox* ySpin_ = nullptr;
    QDoubleSpinBox* zSpin_ = nullptr;
};

// ============================================================
// PropertyPanel - Dynamic property editor panel
// ============================================================

class PropertyPanel : public QScrollArea
{
    Q_OBJECT

public:
    explicit PropertyPanel(QWidget* parent = nullptr)
        : QScrollArea(parent)
    {
        setWidgetResizable(true);

        auto* container = new QWidget(this);
        layout_ = new QVBoxLayout(container);
        layout_->setAlignment(Qt::AlignTop);

        headerLabel_ = new QLabel("No Object Selected", this);
        headerLabel_->setStyleSheet("font-weight: bold; font-size: 14px; padding: 8px;");
        layout_->addWidget(headerLabel_);

        formContainer_ = new QWidget(this);
        formLayout_ = new QFormLayout(formContainer_);
        layout_->addWidget(formContainer_);

        layout_->addStretch();

        setWidget(container);
    }

    void setObject(const SceneObject* obj,
                   std::function<void(const std::string&, Value)> setter) {
        clearProperties();

        if (!obj) {
            headerLabel_->setText("No Object Selected");
            return;
        }

        headerLabel_->setText(QString::fromStdString(obj->type + ": " + obj->id));

        std::map<std::string, std::vector<const PropertyMeta*>> categories;
        for (const auto& meta : obj->meta.properties) {
            categories[meta.category].push_back(&meta);
        }

        for (const auto& [category, props] : categories) {
            if (!category.empty()) {
                auto* groupBox = new QGroupBox(QString::fromStdString(category), formContainer_);
                auto* groupLayout = new QFormLayout(groupBox);

                for (const auto* meta : props) {
                    auto* widget = createPropertyWidget(*meta, *obj, setter);
                    groupLayout->addRow(QString::fromStdString(meta->display_name), widget);
                }

                formLayout_->addRow(groupBox);
            } else {
                for (const auto* meta : props) {
                    auto* widget = createPropertyWidget(*meta, *obj, setter);
                    formLayout_->addRow(QString::fromStdString(meta->display_name), widget);
                }
            }
        }
    }

    void updateValues(const SceneObject& obj) {
        for (auto& [name, widget] : propertyWidgets_) {
            if (auto* map = obj.data.get_if<ValueMap>()) {
                if (auto it = map->find(name); it) {
                    widget->setValue(**it);
                }
            }
        }
    }

signals:
    void propertyChanged(const QString& path, const Value& value);

private:
    PropertyWidget* createPropertyWidget(const PropertyMeta& meta,
                                         const SceneObject& obj,
                                         std::function<void(const std::string&, Value)> setter) {
        auto* widget = new PropertyWidget(meta, this);

        if (auto* map = obj.data.get_if<ValueMap>()) {
            if (auto it = map->find(meta.name); it) {
                widget->setValue(**it);
            }
        }

        connect(widget, &PropertyWidget::valueChanged,
                this, [this, name = meta.name, setter](const Value& val) {
            setter(name, val);
            emit propertyChanged(QString::fromStdString(name), val);
        });

        propertyWidgets_[meta.name] = widget;
        return widget;
    }

    void clearProperties() {
        propertyWidgets_.clear();

        while (formLayout_->count() > 0) {
            auto* item = formLayout_->takeAt(0);
            if (item->widget()) {
                delete item->widget();
            }
            delete item;
        }
    }

    QVBoxLayout* layout_;
    QLabel* headerLabel_;
    QWidget* formContainer_;
    QFormLayout* formLayout_;
    std::map<std::string, PropertyWidget*> propertyWidgets_;
};

// ============================================================
// ObjectTreeWidget - Scene hierarchy tree view
// ============================================================

class ObjectTreeWidget : public QTreeWidget
{
    Q_OBJECT

public:
    explicit ObjectTreeWidget(QWidget* parent = nullptr)
        : QTreeWidget(parent)
    {
        setHeaderLabels({"Name", "Type"});
        setSelectionMode(QAbstractItemView::SingleSelection);
        setAlternatingRowColors(true);
        header()->setSectionResizeMode(0, QHeaderView::Stretch);
        header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

        connect(this, &QTreeWidget::currentItemChanged,
                this, &ObjectTreeWidget::onSelectionChanged);
    }

    void setScene(const SceneState& scene) {
        clear();
        itemMap_.clear();

        for (const auto& [id, obj] : scene.objects) {
            auto* item = new QTreeWidgetItem();

            QString name = QString::fromStdString(id);
            if (auto* map = obj.data.get_if<ValueMap>()) {
                if (auto it = map->find("name"); it) {
                    if (auto* str = (*it)->get_if<std::string>()) {
                        name = QString::fromStdString(*str);
                    }
                }
            }

            item->setText(0, name);
            item->setText(1, QString::fromStdString(obj.type));
            item->setData(0, Qt::UserRole, QString::fromStdString(id));

            QStyle::StandardPixmap icon = QStyle::SP_FileIcon;
            if (obj.type == "Transform") icon = QStyle::SP_DirIcon;
            else if (obj.type == "Light") icon = QStyle::SP_DialogYesButton;
            else if (obj.type == "Camera") icon = QStyle::SP_ComputerIcon;
            item->setIcon(0, style()->standardIcon(icon));

            addTopLevelItem(item);
            itemMap_[id] = item;
        }

        if (!scene.selected_id.empty()) {
            if (auto it = itemMap_.find(scene.selected_id); it != itemMap_.end()) {
                setCurrentItem(it->second);
            }
        }
    }

    void selectObject(const std::string& objectId) {
        if (auto it = itemMap_.find(objectId); it != itemMap_.end()) {
            blockSignals(true);
            setCurrentItem(it->second);
            blockSignals(false);
        }
    }

signals:
    void objectSelected(const QString& objectId);

private slots:
    void onSelectionChanged(QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (current) {
            QString id = current->data(0, Qt::UserRole).toString();
            emit objectSelected(id);
        }
    }

private:
    std::map<std::string, QTreeWidgetItem*> itemMap_;
};

// ============================================================
// EditorMainWindow - Main application window (Qt Widgets version)
// ============================================================

class EditorMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    EditorMainWindow(QWidget* parent = nullptr)
        : QMainWindow(parent)
        , store_(lager::make_store<EditorAction>(
            EditorModel{},
            lager::with_qt_event_loop{*this},
            lager::with_reducer(editor_update)))
    {
        setWindowTitle("Lager Editor - Scene Editor");
        resize(1200, 800);

        setupUI();
        setupActions();
        setupConnections();

        // Initialize engine and sync state
        engine_.initialize_sample_scene();
        auto initialState = engine_.get_initial_state();
        store_.dispatch(actions::SyncFromEngine{payloads::SyncFromEngine{initialState}});

        // Force initial UI update
        updateUI(store_.get());
    }

private:
    void setupUI() {
        auto* splitter = new QSplitter(Qt::Horizontal, this);
        setCentralWidget(splitter);

        objectTree_ = new ObjectTreeWidget(this);
        splitter->addWidget(objectTree_);

        propertyPanel_ = new PropertyPanel(this);
        splitter->addWidget(propertyPanel_);

        splitter->setSizes({300, 700});

        auto* toolbar = addToolBar("Main Toolbar");
        toolbar->setMovable(false);

        undoAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_ArrowBack), "Undo");
        undoAction_->setShortcut(QKeySequence::Undo);

        redoAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_ArrowForward), "Redo");
        redoAction_->setShortcut(QKeySequence::Redo);

        toolbar->addSeparator();

        syncAction_ = toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), "Sync to Engine");

        toolbar->addSeparator();

        historyLabel_ = new QLabel("History: 0 undo / 0 redo", this);
        toolbar->addWidget(historyLabel_);

        statusBar()->showMessage("Ready");
    }

    void setupActions() {
        connect(undoAction_, &QAction::triggered, this, [this]() {
            store_.dispatch(actions::Undo{});
        });

        connect(redoAction_, &QAction::triggered, this, [this]() {
            store_.dispatch(actions::Redo{});
        });

        connect(syncAction_, &QAction::triggered, this, [this]() {
            const auto& model = store_.get();
            std::cout << "[Editor] Syncing to engine, version: "
                      << model.scene.version << std::endl;
            statusBar()->showMessage("Synced to engine", 3000);
        });
    }

    void setupConnections() {
        connect(objectTree_, &ObjectTreeWidget::objectSelected,
                this, [this](const QString& objectId) {
            store_.dispatch(actions::SelectObject{payloads::SelectObject{objectId.toStdString()}});
        });

        store_.watch([this](const EditorModel& model) {
            updateUI(model);
        });
    }

    void updateUI(const EditorModel& model) {
        // Check if scene objects changed (compare version or object count)
        bool sceneChanged = (lastSceneVersion_ != model.scene.version) ||
                           (lastObjectCount_ != model.scene.objects.size());

        // Check if selection changed
        bool selectionChanged = (lastSelectedId_ != model.scene.selected_id);

        // Only rebuild tree if scene structure actually changed
        if (sceneChanged) {
            // Save current selection before rebuilding
            std::string savedSelection = model.scene.selected_id;

            objectTree_->blockSignals(true);
            objectTree_->setScene(model.scene);
            objectTree_->blockSignals(false);

            lastSceneVersion_ = model.scene.version;
            lastObjectCount_ = model.scene.objects.size();
        }

        // Update property panel based on selection
        if (selectionChanged) {
            lastSelectedId_ = model.scene.selected_id;

            if (!model.scene.selected_id.empty()) {
                const SceneObject* obj_ptr = model.scene.objects.find(model.scene.selected_id);
                if (obj_ptr != nullptr) {
                    propertyPanel_->setObject(obj_ptr,
                        [this](const std::string& path, Value val) {
                            store_.dispatch(actions::SetProperty{payloads::SetProperty{path, std::move(val)}});
                        });
                } else {
                    propertyPanel_->setObject(nullptr, nullptr);
                }
            } else {
                propertyPanel_->setObject(nullptr, nullptr);
            }
        } else if (!model.scene.selected_id.empty()) {
            // Selection didn't change, but object data might have - just update values
            const SceneObject* obj_ptr = model.scene.objects.find(model.scene.selected_id);
            if (obj_ptr != nullptr) {
                propertyPanel_->updateValues(*obj_ptr);
            }
        }

        undoAction_->setEnabled(!model.undo_stack.empty());
        redoAction_->setEnabled(!model.redo_stack.empty());

        historyLabel_->setText(QString("History: %1 undo / %2 redo")
            .arg(model.undo_stack.size())
            .arg(model.redo_stack.size()));

        if (model.dirty) {
            statusBar()->showMessage(QString("State changed, version: %1")
                .arg(model.scene.version), 2000);
        }
    }

    lager::store<EditorAction, EditorModel> store_;
    EngineSimulator engine_;

    ObjectTreeWidget* objectTree_;
    PropertyPanel* propertyPanel_;

    QAction* undoAction_;
    QAction* redoAction_;
    QAction* syncAction_;
    QLabel* historyLabel_;

    // State tracking for incremental updates
    std::string lastSelectedId_;
    std::size_t lastSceneVersion_ = 0;
    std::size_t lastObjectCount_ = 0;
};

#include "editor_main.moc"

// ============================================================
// Main function
// ============================================================

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    app.setStyle("Fusion");

    // Dark theme palette
    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, QColor(42, 42, 42));
    darkPalette.setColor(QPalette::AlternateBase, QColor(66, 66, 66));
    darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::HighlightedText, Qt::black);
    app.setPalette(darkPalette);

#if USE_QML_UI
    std::cout << "[Editor] Starting with QML UI..." << std::endl;

    QQmlApplicationEngine engine;
    QQuickStyle::setStyle("Material");

    // TODO: Create QML version of EditorApp class

    engine.load(QUrl::fromLocalFile(
        QString(LAGER_EXT_QML_DIR) + "/main.qml"));

    if (engine.rootObjects().isEmpty()) {
        std::cerr << "[Editor] Failed to load QML!" << std::endl;
        return -1;
    }
#else
    std::cout << "[Editor] Starting with Qt Widgets UI..." << std::endl;

    EditorMainWindow window;
    window.show();
#endif

    return app.exec();
}

