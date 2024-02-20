#include "contentmanagermodel.h"
#include "node.h"
#include "rownode.h"
#include "descriptionnode.h"
#include <zim/error.h>
#include <zim/item.h>
#include "kiwixapp.h"
#include <kiwix/tools.h>

ContentManagerModel::ContentManagerModel(const Downloads* downloads, QObject *parent)
    : QAbstractItemModel(parent)
    , m_downloads(*downloads)
{
    connect(&td, &ThumbnailDownloader::oneThumbnailDownloaded, this, &ContentManagerModel::updateImage);
}

ContentManagerModel::~ContentManagerModel()
{
}

int ContentManagerModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return static_cast<Node*>(parent.internalPointer())->columnCount();
    return rootNode->columnCount();
}

QVariant ContentManagerModel::data(const QModelIndex& index, int role) const
{
    const auto displayRole = role == Qt::DisplayRole;
    const auto additionalInfoRole = role == Qt::UserRole+1;
    if ( (displayRole || additionalInfoRole) && index.isValid() ) {
        const auto item = static_cast<Node*>(index.internalPointer());
        QVariant r = item->data(index.column());
        if ( index.column() != 0 )
            return r;

        r = getThumbnail(r);

        if ( r.type() == QVariant::ByteArray )
            return r;

        const QString faviconUrl = r.toString();
        if ( !faviconUrl.isEmpty() )
            td.addDownload(faviconUrl, item->getBookId());
    }

    return QVariant();
}

Qt::ItemFlags ContentManagerModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags defaultFlags = QAbstractItemModel::flags(index);
    if (index.isValid() && index.parent().isValid()) {
        return defaultFlags & ~Qt::ItemIsDropEnabled & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsSelectable & ~Qt::ItemIsEditable & ~Qt::ItemIsUserCheckable;
    }
    return defaultFlags;
}

QModelIndex ContentManagerModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return QModelIndex();

    RowNode* parentItem;

    if (!parent.isValid()) {
        parentItem = rootNode.get();
    }
    else {
        parentItem = static_cast<RowNode*>(parent.internalPointer());
    }
    auto childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem.get());

    return QModelIndex();
}

QModelIndex ContentManagerModel::parent(const QModelIndex &index) const
{
    if (!index.isValid())
        return QModelIndex();

    auto childItem = static_cast<Node*>(index.internalPointer());
    auto parentItem = childItem->parentItem();

    if (!parentItem || parentItem == rootNode)
        return QModelIndex();

    return createIndex(parentItem->row(), 0, parentItem.get());
}

int ContentManagerModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return zimCount;
}

QVariant ContentManagerModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return QVariant();

    switch (section)
    {
        case 1: return "Name";
        case 2: return "Date";
        case 3: return "Size";
        case 4: return "Content Type";
        default: return QVariant();
    }
}

void ContentManagerModel::setBooksData(const BookInfoList& data)
{
    m_data = data;
    rootNode = std::shared_ptr<RowNode>(new RowNode({tr("Icon"), tr("Name"), tr("Date"), tr("Size"), tr("Content Type"), tr("Download")}, "", std::weak_ptr<RowNode>()));
    setupNodes();
    emit dataChanged(QModelIndex(), QModelIndex());
}

// Returns either data of the thumbnail (as a QByteArray) or a URL (as a
// QString) from where the actual data can be obtained.
QVariant ContentManagerModel::getThumbnail(const QVariant& faviconEntry) const
{
    if ( faviconEntry.type() == QVariant::ByteArray )
        return faviconEntry;

    const auto faviconUrl = faviconEntry.toString();
    return m_iconMap.contains(faviconUrl)
         ? m_iconMap[faviconUrl]
         : faviconEntry;
}

std::shared_ptr<RowNode> ContentManagerModel::createNode(BookInfo bookItem) const
{
    QString id = bookItem["id"].toString();
    const QVariant bookIcon = getThumbnail(bookItem["favicon"]);
    std::weak_ptr<RowNode> weakRoot = rootNode;
    auto rowNodePtr = std::shared_ptr<RowNode>(new
                                    RowNode({bookIcon, bookItem["title"],
                                   bookItem["date"],
                                   QString::fromStdString(kiwix::beautifyFileSize(bookItem["size"].toULongLong())),
                                   bookItem["tags"]
                                   }, id, weakRoot));
    std::weak_ptr<RowNode> weakRowNodePtr = rowNodePtr;
    const auto descNodePtr = std::make_shared<DescriptionNode>(DescriptionNode(bookItem["description"].toString(), weakRowNodePtr));

    rowNodePtr->appendChild(descNodePtr);
    return rowNodePtr;
}

void ContentManagerModel::setupNodes()
{
    beginResetModel();
    bookIdToRowMap.clear();
    for (auto bookItem : m_data) {
        const auto rowNode = createNode(bookItem);

        // Restore download state during model updates (filtering, etc)
        const auto downloadIter = m_downloads.constFind(rowNode->getBookId());
        if ( downloadIter != m_downloads.constEnd() ) {
            rowNode->setDownloadState(downloadIter.value());
        }

        bookIdToRowMap[bookItem["id"].toString()] = rootNode->childCount();
        rootNode->appendChild(rowNode);
    }
    endResetModel();
}

bool ContentManagerModel::hasChildren(const QModelIndex &parent) const
{
    auto item = static_cast<Node*>(parent.internalPointer());
    if (item)
        return item->childCount() > 0;
    return true;
}

bool ContentManagerModel::canFetchMore(const QModelIndex &parent) const
{
    if (parent.isValid())
        return false;
    return (zimCount < m_data.size());
}

void ContentManagerModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid())
        return;
    int remainder = m_data.size() - zimCount;
    int zimsToFetch = qMin(5, remainder);
    beginInsertRows(QModelIndex(), zimCount, zimCount + zimsToFetch - 1);
    zimCount += zimsToFetch;
    endInsertRows();
}

void ContentManagerModel::sort(int column, Qt::SortOrder order)
{
    if (column == 0 || column == 4 || column == 5)
        return;

    QString sortBy = "";
    switch(column) {
        case 1:
            sortBy = "title";
            break;
        case 2:
            sortBy = "date";
            break;
        case 3:
            sortBy = "size";
            break;
        default:
            sortBy = "unsorted";
    }
    KiwixApp::instance()->getContentManager()->setSortBy(sortBy, order == Qt::AscendingOrder);
}

RowNode* ContentManagerModel::getRowNode(size_t row)
{
    return static_cast<RowNode*>(rootNode->child(row).get());
}

void ContentManagerModel::updateImage(QString bookId, QString url, QByteArray imageData)
{
    const auto it = bookIdToRowMap.constFind(bookId);
    if ( it == bookIdToRowMap.constEnd() )
        return;

    const size_t row = it.value();
    const auto item = getRowNode(row);
    item->setIconData(imageData);
    m_iconMap[url] = imageData;
    const QModelIndex index = this->index(row, 0);
    emit dataChanged(index, index);
}

void ContentManagerModel::updateDownload(QString bookId)
{
    const auto it = bookIdToRowMap.constFind(bookId);

    if ( it != bookIdToRowMap.constEnd() ) {
        const size_t row = it.value();
        const QModelIndex newIndex = this->index(row, 5);
        emit dataChanged(newIndex, newIndex);
    }
}


void ContentManagerModel::pauseDownload(QModelIndex index)
{
    emit dataChanged(index, index);
}

void ContentManagerModel::resumeDownload(QModelIndex index)
{
    emit dataChanged(index, index);
}

void ContentManagerModel::removeDownload(QString bookId)
{
    const auto it = bookIdToRowMap.constFind(bookId);
    if ( it == bookIdToRowMap.constEnd() )
        return;

    const size_t row = it.value();
    getRowNode(row)->setDownloadState(nullptr);
    const QModelIndex index = this->index(row, 5);
    emit dataChanged(index, index);
}

