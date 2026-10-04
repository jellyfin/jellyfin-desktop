//
// Created by Tobias Hieta on 20/08/15.
//

#include <QDebug>
#include "CachedRegexMatcher.h"

/////////////////////////////////////////////////////////////////////////////////////////
bool CachedRegexMatcher::addMatcher(const QString& pattern, const QVariant& result)
{
  QRegularExpression matcher(pattern);
  if (!matcher.isValid())
  {
    qWarning() << "Could not compile pattern:" << pattern;
    return false;
  }

  // Remove older mapping if it exists.
  if (!m_allowMultiplePatterns)
  {
    auto newEnd = std::remove_if(m_matcherList.begin(),m_matcherList.end(), [pattern](auto mp)
    {
      return mp.first.pattern() == pattern;
    });
    m_matcherList.erase(newEnd, m_matcherList.end());
  }

  m_matcherList.push_back(qMakePair(matcher, result));
  return true;
}

/////////////////////////////////////////////////////////////////////////////////////////
QVariantList CachedRegexMatcher::match(const QString& input)
{
  // first we check if this match has already happened before
  if (m_matcherCache.contains(input))
    return m_matcherCache.value(input);

  QVariantList matches;

  // otherwise try to iterate our list and find a match
  for(const MatcherValuePair& matcher : m_matcherList)
  {
    QRegularExpressionMatch match = matcher.first.match(input);

    if (match.hasMatch())
    {
      // found match
      QVariant returnValue = matcher.second;

      if (match.lastCapturedIndex() > 0 && matcher.second.typeId() == QMetaType::QString)
      {
        QString value(matcher.second.toString());

        for (int i = 0; i < match.lastCapturedIndex(); i ++)
        {
          QString argFmt = QString("%%1").arg(i + 1);
          if (value.contains(argFmt))
            value = value.arg(match.captured(i + 1));
        }
        returnValue = QVariant(value);
      }

      matches << returnValue;
    }
  }

  if (!matches.isEmpty())
  {
    m_matcherCache.insert(input, matches);
    return matches;
  }

  qDebug() << "No match for:" << input;
  return QVariantList();
}

/////////////////////////////////////////////////////////////////////////////////////////
void CachedRegexMatcher::clear()
{
  m_matcherCache.clear();
  m_matcherList.clear();
}
