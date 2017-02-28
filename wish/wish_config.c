#include "wish_config.h"
#include "wish_debug.h"
#include "wish_fs.h"
#include "bson.h"

#include "bson_visitor.h"

int wish_core_config_load(wish_core_t* core) {
    wish_file_t fd = wish_fs_open(WISH_CORE_CONFIG_DB_NAME);
    if (fd <= 0) {
        WISHDEBUG(LOG_CRITICAL, "Error opening file! Configuration could not be loaded! " WISH_CORE_CONFIG_DB_NAME);
        return -1;
    }
    wish_fs_lseek(fd, 0, WISH_FS_SEEK_SET);
    
    
    int size = 0;
    
    /* First, read in the next mapping id */
    int read_ret = wish_fs_read(fd, (void*) &size, 4);

    if (read_ret != 4) {
        WISHDEBUG(LOG_CRITICAL, "Empty file, or read error in configuration load." WISH_CORE_CONFIG_DB_NAME);
        wish_core_config_save(core);
        return -2;
    }

    if(size>4*1024) {
        WISHDEBUG(LOG_CRITICAL, "Configuration load, file too large (4KiB limit). Found: %i bytes.", size);
        return -3;
    }
    
    bson bs;
    bson_init_size(&bs, size);
    
    /* Go back to start and read the whole file to bson buffer */
    wish_fs_lseek(fd, 0, WISH_FS_SEEK_SET);
    read_ret = wish_fs_read(fd, ((void*)bs.data), size);
    
    if (read_ret != size) {
        WISHDEBUG(LOG_CRITICAL, "Configuration failed to read %i bytes, got %i.", size, read_ret);
    }
    
    wish_fs_close(fd);
    
    bson_visit("Configuration loaded this bson", (char*) bson_data(&bs));

    /* Load content from sandbox file */
#if 0    
    bson_iterator it;
    bson_iterator sit;
    bson_iterator soit;
    bson_iterator pit;
    bson_iterator poit;
    
    if ( bson_find(&it, &bs, "data") != BSON_ARRAY ) {
        // that didn't work
        WISHDEBUG(LOG_CRITICAL, "That didn't work d. %i", bson_find(&sit, &bs, "data"));
        return -4;
    }
    
    // sandbox index
    int si = 0;
    char sindex[21];
    
    while (true) {
        BSON_NUMSTR(sindex, si++);
        
        bson_iterator_subiterator(&it, &sit);
        if ( bson_find_fieldpath_value(sindex, &sit) != BSON_OBJECT ) {
            // that didn't work
            //WISHDEBUG(LOG_CRITICAL, "Not an object at index %s looking for sandboxes.", sindex);
            return -5;
        }
        
        bson_iterator_subiterator(&sit, &soit);

        if ( bson_find_fieldpath_value("name", &soit) != BSON_STRING ) {
            // that didn't work
            WISHDEBUG(LOG_CRITICAL, "That didn't work b.");
            return -6;
        }

        const char* name = bson_iterator_string(&soit);

        //WISHDEBUG(LOG_CRITICAL, "A sandbox name: %s", name);

        bson_iterator_subiterator(&sit, &soit);

        if ( bson_find_fieldpath_value("id", &soit) != BSON_BINDATA || bson_iterator_bin_len(&soit) != WISH_UID_LEN ) {
            // that didn't work
            WISHDEBUG(LOG_CRITICAL, "That didn't work c.");
            return -7;
        }

        const char* id = bson_iterator_bin_data(&soit);

        //WISHDEBUG(LOG_CRITICAL, "A sandbox id: %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);

        bson_iterator_subiterator(&sit, &soit);

        if ( bson_find_fieldpath_value("peers", &soit) != BSON_ARRAY ) {
            // that didn't work
            WISHDEBUG(LOG_CRITICAL, "That didn't work d.");
            return -8;
        }
    }
#endif
    bson_destroy(&bs);
    return 0;
}

int wish_core_config_save(wish_core_t* core) {
    wish_file_t fd;
    int32_t ret = 0;
    fd = wish_fs_open(WISH_CORE_CONFIG_DB_NAME);
    if (fd < 0) {
        /* error */
        WISHDEBUG(LOG_CRITICAL, "could not open configuration db");
        return -1;
    }

    /* FIXME Find if there is already an existing entry for this identity. If
     * there this, delete the old entry */

    /* APPEEND the new BSON document to stable storage file -  */
    ret = wish_fs_lseek(fd, 0, WISH_FS_SEEK_END);
    if (ret < 0) {
        /* error */
        WISHDEBUG(LOG_CRITICAL, "error seeking configuration");
        return -2;
    }
    
    int buf_len = 4*1024;
    char buf[buf_len];
    
    bson bs;
    bson_init_buffer(&bs, buf, buf_len);
    bson_append_string(&bs, "version", "v0.1.1-bogus-dirty-g3dfa");
    bson_finish(&bs);

    ret = wish_fs_write(fd, bson_data(&bs), bson_size(&bs));
    
    bson_destroy(&bs);
    
    if (ret <= 0) {
        /* error */
        WISHDEBUG(LOG_CRITICAL, "error writing configuration");
        return -3;
    }
    wish_fs_close(fd);

    return ret;

}