/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "profilereader.h"

#include <QtCore/QDir>
#include <QtCore/QDebug>

using namespace Qt4ProjectManager;
using namespace Qt4ProjectManager::Internal;

static QString format(const QString &fileName, int lineNo, const QString &msg)
{
    if (lineNo)
        return QString::fromLatin1("%1(%2): %3").arg(fileName, QString::number(lineNo), msg);
    else
        return msg;
}

void ProMessageHandler::parseError(const QString &fileName, int lineNo, const QString &msg)
{
    emit errorFound(format(fileName, lineNo, msg));
}

void ProMessageHandler::configError(const QString &msg)
{
    emit errorFound(msg);
}

void ProMessageHandler::evalError(const QString &fileName, int lineNo, const QString &msg)
{
    if (m_verbose)
        emit errorFound(format(fileName, lineNo, msg));
}

void ProMessageHandler::fileMessage(const QString &)
{
    // we ignore these...
}


ProFileReader::ProFileReader(ProFileOption *option)
    : ProFileParser(ProFileCacheManager::instance()->cache(), this)
    , ProFileEvaluator(option, this, this)
    , m_ignoreLevel(0)
{
}

ProFileReader::~ProFileReader()
{
    foreach (ProFile *pf, m_proFiles)
        pf->deref();
}

bool ProFileReader::readProFile(const QString &fileName)
{
    if (ProFile *pro = parsedProFile(fileName)) {
        bool ok = accept(pro);
        pro->deref();
        return ok;
    }
    return false;
}

void ProFileReader::aboutToEval(ProFile *, ProFile *pro, EvalFileType type)
{
    if (m_ignoreLevel || (type != EvalProjectFile && type != EvalIncludeFile)) {
        m_ignoreLevel++;
    } else if (!m_includeFiles.contains(pro->fileName())) {
        m_includeFiles.insert(pro->fileName(), pro);
        m_proFiles.append(pro);
        pro->ref();
    }
}

void ProFileReader::doneWithEval(ProFile *)
{
    if (m_ignoreLevel)
        m_ignoreLevel--;
}

QList<ProFile*> ProFileReader::includeFiles() const
{
    return m_includeFiles.values();
}

ProFile *ProFileReader::proFileFor(const QString &name)
{
    return m_includeFiles.value(name);
}

ProFileCacheManager *ProFileCacheManager::s_instance = 0;

ProFileCacheManager::ProFileCacheManager(QObject *parent) :
        QObject(parent),
        m_cache(0),
        m_refCount(0)
{
    s_instance = this;
    m_timer.setInterval(5000);
    m_timer.setSingleShot(true);
    connect(&m_timer, SIGNAL(timeout()),
            this, SLOT(clear()));
}

void ProFileCacheManager::incRefCount()
{
    ++m_refCount;
    m_timer.stop();
}

void ProFileCacheManager::decRefCount()
{
    --m_refCount;
    if (!m_refCount)
        m_timer.start();
}

ProFileCacheManager::~ProFileCacheManager()
{
    s_instance = 0;
    clear();
}

ProFileCache *ProFileCacheManager::cache()
{
    if (!m_cache)
        m_cache = new ProFileCache;
    return m_cache;
}

void ProFileCacheManager::clear()
{
    Q_ASSERT(m_refCount == 0);
    // Just deleting the cache will be safe as long as the sequence of
    // obtaining a cache pointer and using it is atomic as far as the main
    // loop is concerned. Use a shared pointer once this is not true anymore.
    delete m_cache;
    m_cache = 0;
}

void ProFileCacheManager::discardFiles(const QString &prefix)
{
    if (m_cache)
        m_cache->discardFiles(prefix);
}

void ProFileCacheManager::discardFile(const QString &fileName)
{
    if (m_cache)
        m_cache->discardFile(fileName);
}
