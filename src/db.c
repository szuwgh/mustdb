

#include "db.h"

Collection* create_collection(const char* schema_name, const char* collection_name)
{
    Collection* collection = (Collection*)malloc(sizeof(Collection));
    if (!collection) return NULL;

    collection->data_path = NULL;
    collection->wal_path = NULL;
    collection->single_file_path = NULL;
    collection->storage_manager = NULL;
    collection->catalog = NULL;

    // Initialize other fields as needed

    return collection;
}