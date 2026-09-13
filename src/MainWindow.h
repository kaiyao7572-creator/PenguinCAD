#pragma once

#include <QMainWindow>

#include <TopoDS_Shape.hxx>

class OcctViewport;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onOpenStep();
    void onExportStl();

private:
    void buildMenus();
    void displayShape(const TopoDS_Shape& shape);

    OcctViewport* m_viewport = nullptr;
    TopoDS_Shape m_currentShape;
};
