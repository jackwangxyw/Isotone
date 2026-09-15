// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The app's settings, in settings.ini under AppPaths::dataDir(). Shell and theme
// settings are properties; anything else is value()/setValue() by key, with
// valueChanged(key), so a page can add a setting without a new property.

#pragma once

#include <QObject>
#include <QSettings>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <memory>

class AppSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // "system", "dark", "light" or "custom".
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    // 0 to 5 into Theme's accents; -1 is customAccent.
    Q_PROPERTY(int accent READ accent WRITE setAccent NOTIFY themeChanged)
    Q_PROPERTY(QString customAccent READ customAccent WRITE setCustomAccent NOTIFY themeChanged)
    // "band" (per band) or "accent".
    Q_PROPERTY(QString bandColours READ bandColours WRITE setBandColours NOTIFY themeChanged)
    Q_PROPERTY(bool sidebarOpen READ sidebarOpen WRITE setSidebarOpen NOTIFY shellChanged)
    Q_PROPERTY(bool panelOpen READ panelOpen WRITE setPanelOpen NOTIFY shellChanged)
    Q_PROPERTY(bool spectrumOn READ spectrumOn WRITE setSpectrumOn NOTIFY shellChanged)
    // Bumped with themeChanged, also when an "appearance/..." value is set.
    Q_PROPERTY(int themeRevision READ themeRevision NOTIFY themeChanged)

public:
    explicit AppSettings(QObject* parent = nullptr);
    ~AppSettings() override;

    QString theme() const;
    void setTheme(const QString& v);
    int accent() const;
    void setAccent(int v);
    QString customAccent() const;
    void setCustomAccent(const QString& v);
    QString bandColours() const;
    void setBandColours(const QString& v);
    bool sidebarOpen() const;
    void setSidebarOpen(bool v);
    bool panelOpen() const;
    void setPanelOpen(bool v);
    bool spectrumOn() const;
    void setSpectrumOn(bool v);
    int themeRevision() const { return theme_revision_; }

    Q_INVOKABLE QVariant value(const QString& key, const QVariant& fallback = QVariant()) const;
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);

signals:
    void themeChanged();
    void shellChanged();
    void valueChanged(const QString& key);

private:
    bool write(const QString& key, const QVariant& value);   // true when it changed

    std::unique_ptr<QSettings> settings_;
    int theme_revision_ = 0;
};
