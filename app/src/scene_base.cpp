#include "qsig/app/scene_base.h"

#include <QFile>
#include <QSaveFile>

namespace qsig::app
{
SceneBase::SceneBase(QObject *parent) : QAbstractListModel(parent)
{
    connect(&m_undo, &QUndoStack::canUndoChanged, this, &SceneBase::undoStateChanged);
    connect(&m_undo, &QUndoStack::canRedoChanged, this, &SceneBase::undoStateChanged);
    connect(&m_undo, &QUndoStack::indexChanged, this, &SceneBase::undoStateChanged);
    connect(&m_undo, &QUndoStack::cleanChanged, this, &SceneBase::modifiedChanged);
}

QString SceneBase::fieldUnit(int, const QString &key) const
{
    if (key.endsWith(QLatin1String("Deg"))) return QStringLiteral("°");
    if (key == QLatin1String("turns")) return {};
    return QStringLiteral("m");
}

int SceneBase::dragMergeId(int row)
{
    if (!m_dragging || m_dragRow != row)
    {
        m_dragging = true;
        m_dragRow = row;
        m_dragSession = (m_dragSession + 1) % 1000000;
    }
    return 1000 + m_dragSession;
}

void SceneBase::endDrag()
{
    m_dragging = false;
    m_dragRow = -1;
}

void SceneBase::forgetFile()
{
    m_fileUrl.clear();
    emit fileUrlChanged();
    emit modifiedChanged();
}

void SceneBase::newScene()
{
    m_undo.clear();
    clearSources();
    forgetFile();
}

bool SceneBase::save(const QUrl &url)
{
    QSaveFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly) || file.write(toJson()) < 0 || !file.commit())
    {
        emit errorOccurred(tr("Impossible d'enregistrer : %1").arg(file.errorString()));
        return false;
    }
    m_undo.setClean();
    m_fileUrl = url;
    emit fileUrlChanged();
    return true;
}

bool SceneBase::load(const QUrl &url)
{
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly))
    {
        emit errorOccurred(tr("Impossible d'ouvrir : %1").arg(file.errorString()));
        return false;
    }
    QString error;
    if (!fromJson(file.readAll(), &error))
    {
        emit errorOccurred(error);
        return false;
    }
    m_fileUrl = url;
    emit fileUrlChanged();
    return true;
}
} // namespace qsig::app
