//
//  DisplayManager.h
//  konvergo
//
//  Created by Lionel CHAZALLON on 28/09/2014.
//
//

#ifndef _DISPLAYMANAGER_H_
#define _DISPLAYMANAGER_H_

#include <QObject>
#include <QMap>
#include <QPoint>
#include <QString>
#include <QSharedPointer>

class QWindow;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Video Modes
class DMVideoMode
{
public:
  int m_id;
  int m_width;
  int m_height;
  int m_bitsPerPixel;
  float m_refreshRate;
  bool m_interlaced;

  int m_privId;

  inline QString getPrettyName()
  {
    QString name;

    name = QString("%1 x%2%3").arg(m_width, 5).arg(m_height, 5).arg((m_interlaced ? "i" : " "));
    name += QString("x %1bpp @%2Hz").arg(m_bitsPerPixel, 2).arg(m_refreshRate);
    return name;
  }
};

typedef QSharedPointer<DMVideoMode> DMVideoModePtr;
typedef QMap<int, DMVideoModePtr> DMVideoModeMap;

///////////////////////////////////////////////////////////////////////////////////////////////////
// Displays
class DMDisplay
{
public:
  int m_id;
  QString m_name;

  int m_privId;

  DMVideoModeMap m_videoModes;
};

typedef QSharedPointer<DMDisplay> DMDisplayPtr;
typedef QMap<int, DMDisplayPtr> DMDisplayMap;

///////////////////////////////////////////////////////////////////////////////////////////////////
// MatchMediaInfo
class DMMatchMediaInfo
{
public:
  DMMatchMediaInfo() : m_refreshRate(0), m_interlaced(false) {};
  DMMatchMediaInfo(float refreshRate, bool interlaced)
    : m_refreshRate(refreshRate), m_interlaced(interlaced) {};

  float m_refreshRate;
  bool m_interlaced;
};

// Matching weights
#define MATCH_WEIGHT_RES 1000

#define MATCH_WEIGHT_REFRESH_RATE_EXACT               200
#define MATCH_WEIGHT_REFRESH_RATE_MULTIPLE            75
#define MATCH_WEIGHT_REFRESH_RATE_CLOSE               50
#define MATCH_WEIGHT_REFRESH_RATE_MULTIPLE_CLOSE      25

#define MATCH_WEIGHT_INTERLACE 10

#define MATCH_WEIGHT_CURRENT 5

///////////////////////////////////////////////////////////////////////////////////////////////////
// VideoMode Weight
class DMVideoModeWeight
{
public:
  float m_weight;
  DMVideoModePtr m_mode;
};

typedef QSharedPointer<DMVideoModeWeight> DMVideoModeWeightPtr;
typedef QMap<int, DMVideoModeWeightPtr> DMVideoModeWeightMap;

///////////////////////////////////////////////////////////////////////////////////////////////////
// DisplayManager
class DisplayManager : public QObject
{
  Q_OBJECT
public:
  explicit DisplayManager(QObject* parent);
  ~DisplayManager() override {}

  DMDisplayMap m_displays;

  // functions that should be implemented on each platform
  virtual bool initialize();
  virtual bool setDisplayMode(int display, int mode) = 0;
  virtual int getCurrentDisplayMode(int display) = 0;
  virtual int getMainDisplay() = 0;
  virtual int getDisplayFromPoint(int x, int y) = 0;

  // extra functions that can be implemented
  virtual void resetRendering() {}
  // Display showing the window. Defaults to the display under the window center.
  virtual int getDisplayFromWindow(QWindow* window);

  // other classes functions
  int findBestMatch(int display, DMMatchMediaInfo& matchInfo);
  DMVideoModePtr getCurrentVideoMode(int display);

  int findBestMode(int display);
  
  bool isValidDisplay(int display);
  bool isValidDisplayMode(int display, int mode);
  int getDisplayFromPoint(const QPoint& pt);
  int findDisplayByName(const QString& name);

private:
  bool isRateMultipleOf(float refresh, float multiple, bool exact = true);
};

typedef QSharedPointer<DisplayManager> DisplayManagerPtr;

#endif /* _DISPLAYMANAGER_H_ */
