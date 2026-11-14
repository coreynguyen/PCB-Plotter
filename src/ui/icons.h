#pragma once
// ============================================================================
// icons.h — PCB Plotter vector icon set (drawn with Direct2D at any DPI,
// recoloured per theme). Designed on a 16x16 grid.
// ============================================================================
#include "win.h"

namespace pcbp::ui {

enum class Icon : int {
    None = 0,
    // file
    New, Open, Save, SaveAs, Cut, Copy, Paste, Print, PrintPreview, About, Undo, Redo, Delete,
    // windows
    GerberView, UniversalView, FactoryModel, MachineProg, VisualAid, Notes, PackageEditor, Locations, Bom,
    // import / export
    ImportCad, ImportGerber, ImportCentroid, ImportBom, Export, Plugin, Fusion,
    // master toolbar
    CompInfo, Select, SelectPart, SelectPackage, SelectTech, PinOne, VirtualOrigin, MoveLabel,
    FindRef, ShowUnassigned, Rot90, Rot180, Rot270,
    // zoom
    ZoomOut, ZoomIn, ZoomRect, Zoom100, ZoomAll, Pan,
    // viewer
    ViewTop, ViewBottom, TopVisible, BottomVisible, AllImages,
    // production
    Assign, Unassign, Digitize, AutoScan, Panelize, Generate, Fiducial, Layers, Report, Machine,
    Grid, Snap, Theme, Settings, Board, Offset, Text, Line, Rect, Circle, Arrow, Picture, BomBlock,
    // gerber workspace
    Measure, Clearance, CropIn, CropOut, Compare, FillSolid, FillOutline, FillSketch, Eye, EyeOff, Solo, Query,
    // v2 additions (keep appending at the end)
    Home, Move, NoRef, SecondSide, VerifyAngle, Question, Warning, Check, Image, Link,
    // v2.3 additions (menus / feedback)
    ErrorX, CloseDoc, CloseAll, Exit, PrintSetup, SelectAll, Keyboard, StatusBar, Units, Sun, Moon, Tag,
    Eraser, Palette, ArrowUp, ArrowDown, Plus, Wand, Flip, Toolbar, Windows, Compact, Recent, Folder,
    // R12 additions
    ArrowLeft, ArrowRight, Maximize,
    // R17c additions
    Polarity, OtherSide,
    // R20 file types (Files tab explorer) + view-mode toggles
    FilePdf, FileDoc, FileSheet, FileCsv, FileZip, FileGerber, FileDrill, FileImage, FileEmail, FileCad, FileBom, FileText, FileGeneric,
    ViewDetails, ViewIcons,
    Count
};

// fg = main stroke colour, accent = highlight colour (ARGB)
void draw_icon(ID2D1RenderTarget* rt, Icon id, const D2D1_RECT_F& r, uint32_t fg, uint32_t accent, float alpha = 1.f);

// For menus: 32bpp premultiplied bitmap of an icon (cached by id/size/theme)
HBITMAP icon_bitmap(Icon id, int size_px);
void icon_cache_clear();

// App icon (generated at runtime for windows when no .ico resource is present)
HICON make_app_icon(int size_px);

} // namespace pcbp::ui
