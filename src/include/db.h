#ifndef DB_H
#define DB_H
#include "storage.h"
#include "catalog.h"

typedef struct CollectionOptions
{
} CollectionOptions;

typedef struct Collection
{
    const char* data_path;
    const char* wal_path;
    const char* single_file_path;
    StorageManager* storage_manager;
    Catalog* catalog;
} Collection;

Collection* create_collection(const char* schema_name, const char* collection_name);

#endif // MUSTDB_H