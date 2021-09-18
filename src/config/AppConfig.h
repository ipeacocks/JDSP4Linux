/*
 *  This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 *  ThePBone <tim.schneeberger(at)outlook.de> (c) 2020
 */
#ifndef APPCONFIGWRAPPER_H
#define APPCONFIGWRAPPER_H

#include "ConfigContainer.h"
#include "ConfigIO.h"
#include "utils/Common.h"
#include "utils/Log.h"

#include <QObject>
#include <QMetaEnum>

using namespace std;

#define DEFINE_KEY(name, defaultValue) \
    definitions[name] = QVariant(defaultValue);

class AppConfig :
	public QObject
{
	Q_OBJECT

public:
	static AppConfig &instance()
	{
		static AppConfig _instance;
		return _instance;
	}

	AppConfig(AppConfig const &)       = delete;
	void operator= (AppConfig const &) = delete;

	AppConfig()
	{
		_appconf = new ConfigContainer();

        DEFINE_KEY(LiveprogAutoExtract, true);

        DEFINE_KEY(Theme, "Fusion");
        DEFINE_KEY(ThemeColors, "Default");
        DEFINE_KEY(ThemeColorsCustom, "");
        DEFINE_KEY(ThemeColorsCustomWhiteIcons, false);

        DEFINE_KEY(TrayIconEnabled, true);
        DEFINE_KEY(TrayIconMenu, "");

        DEFINE_KEY(SpectrumEnabled, false);
        DEFINE_KEY(SpectrumGrid, false);
        DEFINE_KEY(SpectrumBands, 0);
        DEFINE_KEY(SpectrumMinFreq, 0);
        DEFINE_KEY(SpectrumMaxFreq, 0);
        DEFINE_KEY(SpectrumTheme, 0);
        DEFINE_KEY(SpectrumRefresh, 0);
        DEFINE_KEY(SpectrumMultiplier, 0);

        DEFINE_KEY(EqualizerShowHandles, false);

        DEFINE_KEY(SetupDone, false);
        DEFINE_KEY(ExecutablePath, "");
        DEFINE_KEY(VdcLastDatabaseId, -1);

        DEFINE_KEY(AudioOutputUseDefault, true);
        DEFINE_KEY(AudioOutputDevice, "");
        DEFINE_KEY(AudioAppBlocklist, QStringList());

        connect(this, &AppConfig::updated, this, &AppConfig::notify);

		load();
	}

    enum Key {
        LiveprogAutoExtract,

        Theme,
        ThemeColors,
        ThemeColorsCustom,
        ThemeColorsCustomWhiteIcons,

        TrayIconEnabled,
        TrayIconMenu,

        SpectrumEnabled,
        SpectrumGrid,
        SpectrumBands,
        SpectrumMinFreq,
        SpectrumMaxFreq,
        SpectrumTheme,
        SpectrumRefresh,
        SpectrumMultiplier,

        EqualizerShowHandles,

        SetupDone,
        ExecutablePath,
        VdcLastDatabaseId,

        AudioOutputUseDefault,
        AudioOutputDevice,
        AudioAppBlocklist
    };
    Q_ENUM(Key);

    void set(const Key &key,
             const QStringList &value)
    {
        set(key, value.join(';'));
    }

    void set(const Key &key,
             const QVariant &value)
    {
        _appconf->setValue(QVariant::fromValue(key).toString(), value);
        emit updated(key, value);
        save();
    }

    template<class T>
    static T convertVariant(QVariant variant){
        if constexpr (std::is_same_v<T, QVariant>) {
            return variant;
        }
        if constexpr (std::is_same_v<T, std::string>) {
            return variant.toString().toStdString();
        }
        if constexpr (std::is_same_v<T, QString>) {
            return variant.toString();
        }
        if constexpr (std::is_same_v<T, QStringList>) {
            return variant.toString().split(';');
        }
        if constexpr (std::is_same_v<T, int>) {
            return variant.toInt();
        }
        if constexpr (std::is_same_v<T, float>) {
            return variant.toFloat();
        }
        if constexpr (std::is_same_v<T, bool>) {
            return variant.toBool();
        }

        Log::error("AppConfig::convertVariant<T>: Unknown type T");
    }

    template<class T>
    T get(const Key &key)
    {
        bool exists;
        auto skey = QVariant::fromValue(key).toString();
        auto variant = _appconf->getVariant(skey, true, &exists);

        QVariant defaultValue = definitions[key];

        if(!exists)
        {
            return convertVariant<T>(defaultValue);
        }

        return convertVariant<T>(variant);
    }

	QString getDspConfPath()
	{
		return QString("%1/.config/jamesdsp/audio.conf").arg(QDir::homePath());
	}

	QString getPath(QString subdir = "")
	{
		return QString("%1/.config/jamesdsp/%2").arg(QDir::homePath()).arg(subdir);
	}

	void setIrsPath(const QString &npath)
	{
        _appconf->setValue("ConvolverDefaultPath", QVariant(QString("\"%1\"").arg(npath)));
		save();
	}

	QString getIrsPath()
	{
        QString irs_path = chopFirstLastChar(_appconf->getString("ConvolverDefaultPath", false));

		if (irs_path.length() < 2)
		{
			return QString("%1/IRS").arg(QDir::homePath());
		}

		return irs_path;
	}

	void setDDCPath(const QString &npath)
	{
        _appconf->setValue("VdcDefaultPath", QVariant(QString("\"%1\"").arg(npath)));
		save();
	}

	QString getDDCPath()
	{
        QString irs_path = chopFirstLastChar(_appconf->getString("VdcDefaultPath", false));

		if (irs_path.length() < 2)
		{
			return QString("%1/DDC").arg(QDir::homePath());
		}

		return irs_path;
	}

	void setLiveprogPath(const QString &npath)
	{
        _appconf->setValue("LiveprogDefaultPath", QVariant(QString("\"%1\"").arg(npath)));
		save();
	}

	QString getLiveprogPath()
	{
		QString absolute = QFileInfo(getDspConfPath()).absoluteDir().absolutePath();
        QString lp_path  = chopFirstLastChar(_appconf->getString("LiveprogDefaultPath", false));

		if (lp_path.length() < 2)
		{
			QDir(absolute).mkdir("liveprog");
			QString defaultstr = QString("%1/liveprog").arg(absolute);
			setLiveprogPath(defaultstr);
			return defaultstr;
		}

		return lp_path;
	}

	void save()
	{
        auto file = QString("%1/.config/jamesdsp/application.conf").arg(QDir::homePath());
		ConfigIO::writeFile(file, _appconf->getConfigMap());
	}

	void load()
	{
        auto map = ConfigIO::readFile(QString("%1/.config/jamesdsp/application.conf").arg(QDir::homePath()));
		_appconf->setConfigMap(map);

        for(const auto& key : map.keys())
        {
            auto ekey  = static_cast<Key>(QMetaEnum::fromType<Key>().keyToValue(key.toLocal8Bit().constData()));
            emit updated(ekey, map[key]);
        }
	}

	QString getGraphicEQConfigFilePath()
	{
        return pathAppend(QFileInfo(getDspConfPath()).absoluteDir().absolutePath(), "graphiceq.conf");
	}

private slots:
    void notify(const Key& key, const QVariant& value)
    {
        switch(key)
        {
        case Theme:
        case ThemeColors:
        case ThemeColorsCustom:
        case ThemeColorsCustomWhiteIcons:
            emit themeChanged(key, value);
            break;
        case SpectrumEnabled:
        case SpectrumBands:
        case SpectrumGrid:
        case SpectrumMinFreq:
        case SpectrumMaxFreq:
        case SpectrumMultiplier:
        case SpectrumTheme:
            emit spectrumChanged(false);
            break;
        case SpectrumRefresh:
            emit spectrumChanged(true);
            break;
        default:
            break;
        }
    }

signals:
    void spectrumChanged(bool needReload);
    void themeChanged(const Key&, const QVariant&);
    void updated(const Key&, const QVariant&);

private:
    QMap<Key, QVariant> definitions;
	ConfigContainer *_appconf;
};

#endif // APPCONFIGWRAPPER_H