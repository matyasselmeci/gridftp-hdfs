/*
 * Portions of this file Copyright 1999-2005 University of Chicago
 * Portions of this file Copyright 1999-2005 The University of Southern California.
 *
 * This file or a portion of this file is licensed under the
 * terms of the Globus Toolkit Public License, found at
 * http://www.globus.org/toolkit/download/license.html.
 * If you redistribute this file, with or without
 * modifications, you must include this notice in the file.
 */

#include "globus_gridftp_server.h"

#include <grp.h>
#include <pwd.h>
#include <hdfs.h>

static const int default_id = 00;

/*@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
 *  ADD ADDITIONAL INCLUDES HERE
 *@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@*/

static
globus_version_t local_version =
{
    0, /* major version number */
    1, /* minor version number */
    1222387054,
    0 /* branch ID */
};

typedef struct globus_l_gfs_hdfs_handle_s
{
    char *                              pathname;
    hdfsFS                              fs;
    hdfsFile                            fd;
    globus_size_t                       block_size;
    globus_off_t                        block_length;
    globus_off_t                        offset;
    globus_bool_t                       done;
    globus_gfs_operation_t              op;
    globus_byte_t *                     buffer;
    globus_off_t *                      offsets;
    globus_size_t *                     nbytes;
    short *                             used;
    int                                 optimal_count;
    int                                 buffer_count;
    int                                 outstanding;
    globus_mutex_t                      mutex;
} globus_l_gfs_hdfs_handle_t;

char err_msg[256];
static int local_io_block_size = 0;
static int local_io_count = 0;

/*************************************************************************
 *  start
 *  -----
 *  This function is called when a new session is initialized, ie a user 
 *  connectes to the server.  This hook gives the dsi an oppertunity to
 *  set internal state that will be threaded through to all other
 *  function calls associated with this session.  And an oppertunity to
 *  reject the user.
 *
 *  finished_info.info.session.session_arg should be set to an DSI
 *  defined data structure.  This pointer will be passed as the void *
 *  user_arg parameter to all other interface functions.
 * 
 *  NOTE: at nice wrapper function should exist that hides the details 
 *        of the finished_info structure, but it currently does not.  
 *        The DSI developer should jsut follow this template for now
 ************************************************************************/
static
void
globus_l_gfs_hdfs_start(
    globus_gfs_operation_t              op,
    globus_gfs_session_info_t *         session_info)
{
    globus_l_gfs_hdfs_handle_t *       hdfs_handle;
    globus_gfs_finished_info_t          finished_info;
    GlobusGFSName(globus_l_gfs_hdfs_start);

    hdfs_handle = (globus_l_gfs_hdfs_handle_t *)
        globus_malloc(sizeof(globus_l_gfs_hdfs_handle_t));

    memset(&finished_info, '\0', sizeof(globus_gfs_finished_info_t));
    finished_info.type = GLOBUS_GFS_OP_SESSION_START;
    finished_info.result = GLOBUS_SUCCESS;
    finished_info.info.session.session_arg = hdfs_handle;
    finished_info.info.session.username = session_info->username;
    finished_info.info.session.home_dir = "/";

    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);

    hdfs_handle->fs = hdfsConnect("node001", 9000);
    if (!hdfs_handle->fs) {
        finished_info.result = GLOBUS_FAILURE;
        globus_gridftp_server_operation_finished(
            op, GLOBUS_FAILURE, &finished_info);
        return;
    }

    globus_gridftp_server_operation_finished(
        op, GLOBUS_SUCCESS, &finished_info);
}

/*************************************************************************
 *  destroy
 *  -------
 *  This is called when a session ends, ie client quits or disconnects.
 *  The dsi should clean up all memory they associated wit the session
 *  here. 
 ************************************************************************/
static
void
globus_l_gfs_hdfs_destroy(
    void *                              user_arg)
{
    globus_l_gfs_hdfs_handle_t *       hdfs_handle;
    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;
    hdfsDisconnect(hdfs_handle->fs);
    globus_free(hdfs_handle);
}

void
globus_l_gfs_file_copy_stat(
    globus_gfs_stat_t *                 stat_object,
    hdfsFileInfo *                      fileInfo,
    const char *                        filename,
    const char *                        symlink_target)
{
    GlobusGFSName(globus_l_gfs_file_copy_stat);

    stat_object->mode     = (fileInfo->mKind == kObjectKindDirectory) ? (S_IFDIR | 0777) :  (S_IFREG | 0666);
    stat_object->nlink    = (fileInfo->mKind == kObjectKindDirectory) ? 0 : 1;
    stat_object->uid      = default_id;
    stat_object->gid      = default_id;
    stat_object->size     = (fileInfo->mKind == kObjectKindDirectory) ? 4096 : fileInfo->mSize;
    stat_object->mtime    = fileInfo->mLastMod;
    stat_object->atime    = fileInfo->mLastMod;
    stat_object->ctime    = fileInfo->mLastMod;
    stat_object->dev      = 0;
    stat_object->ino      = 0;

    if(filename && *filename)
    {
        stat_object->name = strdup(filename);
    }
    else
    {
        stat_object->name = NULL;
    }
    if(symlink_target && *symlink_target)
    {
        stat_object->symlink_target = strdup(symlink_target);
    }
    else
    {
        stat_object->symlink_target = NULL;
    }
}

static
void
globus_l_gfs_file_destroy_stat(
    globus_gfs_stat_t *                 stat_array,
    int                                 stat_count)
{
    int                                 i;
    GlobusGFSName(globus_l_gfs_file_destroy_stat);

    for(i = 0; i < stat_count; i++)
    {
        if(stat_array[i].name != NULL)
        {
            globus_free(stat_array[i].name);
        }
        if(stat_array[i].symlink_target != NULL)
        {
            globus_free(stat_array[i].symlink_target);
        }
    }
    globus_free(stat_array);
}

/* basepath and filename must be MAXPATHLEN long
 * the pathname may be absolute or relative, basepath will be the same */
static
void
globus_l_gfs_file_partition_path(
    const char *                        pathname,
    char *                              basepath,
    char *                              filename)
{
    char                                buf[MAXPATHLEN];
    char *                              filepart;
    GlobusGFSName(globus_l_gfs_file_partition_path);

    strncpy(buf, pathname, MAXPATHLEN);
    buf[MAXPATHLEN - 1] = '\0';

    filepart = strrchr(buf, '/');
    while(filepart && !*(filepart + 1) && filepart != buf)
    {
        *filepart = '\0';
        filepart = strrchr(buf, '/');
    }

    if(!filepart)
    {
        strcpy(filename, buf);
        basepath[0] = '\0';
    }
    else
    {
        if(filepart == buf)
        {
            if(!*(filepart + 1))
            {
                basepath[0] = '\0';
                filename[0] = '/';
                filename[1] = '\0';
            }
            else
            {
                *filepart++ = '\0';
                basepath[0] = '/';
                basepath[1] = '\0';
                strcpy(filename, filepart);
            }
        }
        else
        {
            *filepart++ = '\0';
            strcpy(basepath, buf);
            strcpy(filename, filepart);
        }
    }
}

/*************************************************************************
 *  stat
 *  ----
 *  This interface function is called whenever the server needs 
 *  information about a given file or resource.  It is called then an
 *  LIST is sent by the client, when the server needs to verify that 
 *  a file exists and has the proper permissions, etc.
 ************************************************************************/
static
void
globus_l_gfs_hdfs_stat(
    globus_gfs_operation_t              op,
    globus_gfs_stat_info_t *            stat_info,
    void *                              user_arg)
{
    globus_result_t                     result;
    globus_gfs_stat_t *                 stat_array;
    int                                 stat_count = 0;
    DIR *                               dir;
    char                                basepath[MAXPATHLEN];
    char                                filename[MAXPATHLEN];
    char                                symlink_target[MAXPATHLEN];
    char *                              PathName;
    globus_l_gfs_hdfs_handle_t *       hdfs_handle;
    GlobusGFSName(globus_l_gfs_hdfs_stat);
    PathName=stat_info->pathname;
    
    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;

    hdfsFileInfo * fileInfo = NULL;

    /* lstat is the same as stat when not operating on a link */
    if((fileInfo = hdfsGetPathInfo(hdfs_handle->fs, PathName)) != NULL);
    {
        result = GlobusGFSErrorSystemError("stat", errno);
        goto error_stat1;
    }

    mode_t mode = (fileInfo->mKind == kObjectKindDirectory) ? (S_IFDIR | 0777) :  (S_IFREG | 0666);

    globus_l_gfs_file_partition_path(PathName, basepath, filename);
    
    if(!S_ISDIR(mode) || stat_info->file_only)
    {
        stat_array = (globus_gfs_stat_t *)
            globus_malloc(sizeof(globus_gfs_stat_t));
        if(!stat_array)
        {
            result = GlobusGFSErrorMemory("stat_array");
            goto error_alloc1;
        }
        
        globus_l_gfs_file_copy_stat(
            stat_array, fileInfo, filename, symlink_target);
        hdfsFreeFileInfo(fileInfo, 1);
        stat_count = 1;
    }
    else
    {
        int                             i;
    
        hdfsFileInfo * dir = hdfsListDirectory(hdfs_handle->fs, PathName, &stat_count);
        if(dir == NULL)
        {
            result = GlobusGFSErrorSystemError("opendir", errno);
            goto error_open;
        }
        
        stat_array = (globus_gfs_stat_t *)
            globus_malloc(sizeof(globus_gfs_stat_t) * stat_count);
        if(!stat_array)
        {
            result = GlobusGFSErrorMemory("stat_array");
            goto error_alloc2;
        }

        for(i = 0; stat_count; i++)
        {
            globus_l_gfs_file_copy_stat(
                &stat_array[i], &dir[i], dir[i].mName, 0);
        }
        hdfsFreeFileInfo(dir, stat_count);
        
        if(i != stat_count)
        {
            result = GlobusGFSErrorSystemError("readdir", errno);
            goto error_read;
        }
        
    }
    
    globus_gridftp_server_finished_stat(
        op, GLOBUS_SUCCESS, stat_array, stat_count);
    
    
    globus_l_gfs_file_destroy_stat(stat_array, stat_count);
    return;

error_read:
    globus_l_gfs_file_destroy_stat(stat_array, stat_count);
    
error_alloc2:
    closedir(dir);
    
error_open:
error_alloc1:
error_stat1:
    globus_gridftp_server_finished_stat(op, result, NULL, 0);

/*    GlobusGFSFileDebugExitWithError();  */
}


/*************************************************************************
 *  command
 *  -------
 *  This interface function is called when the client sends a 'command'.
 *  commands are such things as mkdir, remdir, delete.  The complete
 *  enumeration is below.
 *
 *  To determine which command is being requested look at:
 *      cmd_info->command
 *
 *      GLOBUS_GFS_CMD_MKD = 1,
 *      GLOBUS_GFS_CMD_RMD,
 *      GLOBUS_GFS_CMD_DELE,
 *      GLOBUS_GFS_CMD_RNTO,
 *      GLOBUS_GFS_CMD_RNFR,
 *      GLOBUS_GFS_CMD_CKSM,
 *      GLOBUS_GFS_CMD_SITE_CHMOD,
 *      GLOBUS_GFS_CMD_SITE_DSI
 ************************************************************************/
static
void
globus_l_gfs_hdfs_command(
    globus_gfs_operation_t              op,
    globus_gfs_command_info_t *         cmd_info,
    void *                              user_arg)
{
    globus_l_gfs_hdfs_handle_t *       hdfs_handle;
     printf("Globus HDFS command.\n");
    GlobusGFSName(globus_l_gfs_hdfs_command);

    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;

    int success = GLOBUS_FALSE;
    switch (cmd_info->command) {
    case GLOBUS_GFS_CMD_MKD:
        break;
    case GLOBUS_GFS_CMD_RMD:
        break;
    case GLOBUS_GFS_CMD_DELE:
        break;
    case GLOBUS_GFS_CMD_RNTO:
        break;
    case GLOBUS_GFS_CMD_RNFR:
        break;
    case GLOBUS_GFS_CMD_CKSM:
        break;
    case GLOBUS_GFS_CMD_SITE_CHMOD:
        break;
    case GLOBUS_GFS_CMD_SITE_DSI:
        break;
    case GLOBUS_GFS_CMD_SITE_RDEL:
        break;
    }
    if (success)
        globus_gridftp_server_finished_command(op, GLOBUS_SUCCESS, GLOBUS_NULL);
    else
        globus_gridftp_server_finished_command(op, GLOBUS_FAILURE, GLOBUS_NULL);
}

/* receive file from client */

static
void
globus_l_gfs_hdfs_write_to_storage(
    globus_l_gfs_hdfs_handle_t *      hdfs_handle);

static
globus_result_t
globus_l_gfs_hdfs_dump_buffers(
    globus_l_gfs_hdfs_handle_t *      hdfs_handle
) {
    globus_off_t * offsets = hdfs_handle->offsets;
    globus_size_t * nbytes = hdfs_handle->nbytes;
    globus_size_t bytes_written = 0;
    int i, wrote_something;
    int cnt = hdfs_handle->buffer_count;
    GlobusGFSName(globus_l_gfs_hdfs_dump_buffers);
    globus_result_t rc = GLOBUS_SUCCESS;

    wrote_something=1;
    while (wrote_something == 1) {
        wrote_something=0;
        for (i=0; i<cnt; i++) {
            if (hdfs_handle->used[i] == 1 && offsets[i] == hdfs_handle->offset) {
                //printf("Flushing %d bytes at offset %d from buffer %d.\n", nbytes[i], hdfs_handle->offset, i);
                bytes_written = hdfsWrite(hdfs_handle->fs, hdfs_handle->fd, hdfs_handle->buffer+i*hdfs_handle->block_size, nbytes[i]*sizeof(globus_byte_t));
                wrote_something = 1;
                if (bytes_written != nbytes[i]) {
                    rc = GlobusGFSErrorGeneric("write() fail");
                    hdfs_handle->done = GLOBUS_TRUE;
                    break;
                }
                hdfs_handle->used[i] = 0;
                hdfs_handle->offset += bytes_written;
            }
        }
    }
    return rc;
}

static
globus_result_t globus_l_gfs_hdfs_store_buffer(globus_l_gfs_hdfs_handle_t * hdfs_handle, globus_byte_t* buffer, globus_off_t offset, globus_size_t nbytes) {
    GlobusGFSName(globus_l_gfs_hdfs_store_buffer);
    globus_result_t rc = GLOBUS_SUCCESS;
    int i, cnt = hdfs_handle->buffer_count;
    short wrote_something = 0;
    for (i = 0; i<cnt; i++) {
        if (hdfs_handle->used[i] == 0) {
            //printf("Stored some bytes in buffer %d; offset %d.\n", i, offset);
            hdfs_handle->nbytes[i] = nbytes;
            hdfs_handle->offsets[i] = offset;
            hdfs_handle->used[i] = 1;
            wrote_something=1;
            memcpy(hdfs_handle->buffer+i*hdfs_handle->block_size, buffer, nbytes*sizeof(globus_byte_t));
            break;
        }
    }
    if (wrote_something == 0) {
        hdfs_handle->buffer_count += 1;
        if (hdfs_handle->buffer_count == 100) {
            rc = GlobusGFSErrorGeneric("store_buffer() failed.");
        } else {
            hdfs_handle->nbytes = globus_realloc(hdfs_handle->nbytes, hdfs_handle->buffer_count*sizeof(globus_size_t));
            hdfs_handle->offsets = globus_realloc(hdfs_handle->offsets, hdfs_handle->buffer_count*sizeof(globus_off_t));
            hdfs_handle->used = globus_realloc(hdfs_handle->used, hdfs_handle->buffer_count*sizeof(short));
            hdfs_handle->used[hdfs_handle->buffer_count-1] = 1;
            hdfs_handle->buffer = globus_realloc(hdfs_handle->buffer, hdfs_handle->buffer_count*hdfs_handle->block_size*sizeof(globus_byte_t));
            if (hdfs_handle->buffer == NULL || hdfs_handle->nbytes==NULL || hdfs_handle->offsets==NULL || hdfs_handle->used==NULL) {  
                rc = GlobusGFSErrorGeneric("Memory allocation error.");
                globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            }
            memcpy(hdfs_handle->buffer+(hdfs_handle->buffer_count-1)*hdfs_handle->block_size, buffer, nbytes*sizeof(globus_byte_t));
            hdfs_handle->nbytes[hdfs_handle->buffer_count-1] = nbytes;
            hdfs_handle->offsets[hdfs_handle->buffer_count-1] = offset;
            //printf("Stored some bytes in buffer %d; offset %d.\n", hdfs_handle->buffer_count-1, offset);
        }
        //printf("Increasing number of buffers to %d.\n", hdfs_handle->buffer_count);
    }

    return rc;
}


static
void 
globus_l_gfs_hdfs_write_to_storage_cb(
    globus_gfs_operation_t              op,
    globus_result_t                     result,
    globus_byte_t *                     buffer,
    globus_size_t                       nbytes,
    globus_off_t                        offset,
    globus_bool_t                       eof,
    void *                              user_arg)
{
    globus_result_t                     rc; 
    globus_l_gfs_hdfs_handle_t *        hdfs_handle;
                                                                                                                                           
    GlobusGFSName(globus_l_gfs_hdfs_write_to_storage_cb);
    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;

    globus_mutex_lock(&hdfs_handle->mutex);
    rc = GLOBUS_SUCCESS;
    if (result != GLOBUS_SUCCESS)
    {
        rc = GlobusGFSErrorGeneric("call back fail");
        hdfs_handle->done = GLOBUS_TRUE;
    }
    else if (eof)
    {
        hdfs_handle->done = GLOBUS_TRUE;
    }

    if (nbytes > 0)
    {
        rc = globus_l_gfs_hdfs_store_buffer(hdfs_handle, buffer, offset, nbytes);
        if (rc != GLOBUS_SUCCESS) {
            hdfs_handle->done = GLOBUS_TRUE;
        } else {
            rc = globus_l_gfs_hdfs_dump_buffers(hdfs_handle);
            globus_gridftp_server_update_bytes_written(op, offset, nbytes);
        }
        if (nbytes != local_io_block_size)
        {
            if (local_io_block_size != 0)
            {
                sprintf(err_msg,"receive %d blocks of size %d bytes\n",
                                local_io_count,local_io_block_size);
                globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
            }
            local_io_block_size = nbytes;
            local_io_count=1;
        }
        else
        {
            local_io_count++;
        }
    }
    globus_free(buffer);

    hdfs_handle->outstanding--;
    if (! hdfs_handle->done)
    {
        globus_l_gfs_hdfs_write_to_storage(hdfs_handle);
    }
    else if (hdfs_handle->outstanding == 0) 
    {
        globus_free(hdfs_handle->buffer);
        globus_free(hdfs_handle->used);
        globus_free(hdfs_handle->nbytes);
        globus_free(hdfs_handle->offsets);
        if (hdfsCloseFile(hdfs_handle->fs, hdfs_handle->fd) == -1) 
        {
             rc = GlobusGFSErrorGeneric("close() fail");
        }
        sprintf(err_msg,"receive %d blocks of size %d bytes\n",
                        local_io_count,local_io_block_size);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
        local_io_count = 0;
        local_io_block_size = 0;

        globus_gridftp_server_finished_transfer(op, rc);
    }
    globus_mutex_unlock(&hdfs_handle->mutex);
}

static
void
globus_l_gfs_hdfs_write_to_storage(
    globus_l_gfs_hdfs_handle_t *      hdfs_handle)
{
    globus_byte_t *                     buffer;
    globus_result_t                     rc;

    GlobusGFSName(globus_l_gfs_hdfs_write_to_storage);
    //printf("Globus write_to_storage; outstanding %d, optimal %d.\n", hdfs_handle->outstanding, hdfs_handle->optimal_count);
    while (hdfs_handle->outstanding < hdfs_handle->optimal_count) 
    {
        buffer = globus_malloc(hdfs_handle->block_size);
        if (buffer == NULL)
        {
            rc = GlobusGFSErrorGeneric("fail to allocate buffer");
            globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            return;
        }
        rc = globus_gridftp_server_register_read(hdfs_handle->op,
                                       buffer,
                                       hdfs_handle->block_size,
                                       globus_l_gfs_hdfs_write_to_storage_cb,
                                       hdfs_handle);
        if (rc != GLOBUS_SUCCESS)
        {
            rc = GlobusGFSErrorGeneric("globus_gridftp_server_register_read() fail");
            globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            return;
        }
        hdfs_handle->outstanding++;
    }
    return; 
}



/*************************************************************************
 *  recv
 *  ----
 *  This interface function is called when the client requests that a
 *  file be transfered to the server.
 *
 *  To receive a file the following functions will be used in roughly
 *  the presented order.  They are doced in more detail with the
 *  gridftp server documentation.
 *
 *      globus_gridftp_server_begin_transfer();
 *      globus_gridftp_server_register_read();
 *      globus_gridftp_server_finished_transfer();
 *
 ************************************************************************/
static
void
globus_l_gfs_hdfs_recv(
    globus_gfs_operation_t              op,
    globus_gfs_transfer_info_t *        transfer_info,
    void *                              user_arg)
{
    globus_l_gfs_hdfs_handle_t *        hdfs_handle;
    globus_result_t                     rc; 

    //printf("hdfs recv.\n");
    GlobusGFSName(globus_l_gfs_hdfs_recv);

    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;

    hdfs_handle->pathname = transfer_info->pathname;

    hdfs_handle->op = op;
    hdfs_handle->outstanding = 0;
    hdfs_handle->done = GLOBUS_FALSE;
    globus_gridftp_server_get_block_size(op, &hdfs_handle->block_size); 

    globus_gridftp_server_get_write_range(hdfs_handle->op,
                                          &hdfs_handle->offset,
                                          &hdfs_handle->block_length);

    if (hdfs_handle->offset != 0) {
        rc = GlobusGFSErrorGeneric("Non-zero offsets are not supported.");
        globus_gridftp_server_finished_transfer(op, rc);
        return;
    }

    globus_gridftp_server_begin_transfer(hdfs_handle->op, 0, hdfs_handle);
    if (hdfsExists(hdfs_handle->fs, hdfs_handle->pathname) == 0)
    {
        //printf("Opened the hdfs handle O_WRONLY.\n");
        hdfs_handle->fd = hdfsOpenFile(hdfs_handle->fs, hdfs_handle->pathname,
            O_WRONLY, 0, 1, 0);
    }
    else
    {
        //printf("Opened the hdfs handle O_CREAT.\n");
        hdfs_handle->fd = hdfsOpenFile(hdfs_handle->fs, hdfs_handle->pathname,
                                 O_WRONLY, 0, 1, 0);
    }
    if (!hdfs_handle->fd)
    {
        rc = GlobusGFSErrorGeneric("open() fail");
        globus_gridftp_server_finished_transfer(op, rc);
        return;
    }

    globus_gridftp_server_get_optimal_concurrency(hdfs_handle->op,
                                                  &hdfs_handle->optimal_count);
    hdfs_handle->buffer_count = hdfs_handle->optimal_count;
    hdfs_handle->nbytes = globus_malloc(hdfs_handle->buffer_count*sizeof(globus_size_t));
    hdfs_handle->offsets = globus_malloc(hdfs_handle->buffer_count*sizeof(globus_off_t));
    hdfs_handle->used = globus_malloc(hdfs_handle->buffer_count*sizeof(short));
    int i;
    for (i=0; i<hdfs_handle->buffer_count; i++)
        hdfs_handle->used[i] = 0;
    hdfs_handle->buffer = globus_malloc(hdfs_handle->buffer_count*hdfs_handle->block_size*sizeof(globus_byte_t));
    if (hdfs_handle->buffer == NULL || hdfs_handle->nbytes==NULL || hdfs_handle->offsets==NULL || hdfs_handle->used==NULL) {  
        rc = GlobusGFSErrorGeneric("Memory allocation error.");
        globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
    }

    globus_mutex_lock(&hdfs_handle->mutex);
    globus_l_gfs_hdfs_write_to_storage(hdfs_handle);
    globus_mutex_unlock(&hdfs_handle->mutex);
    return;
}

/* send files to client */

static
void
globus_l_gfs_hdfs_read_from_storage(
    globus_l_gfs_hdfs_handle_t *      hdfs_handle);

static
void
globus_l_gfs_hdfs_read_from_storage_cb(
    globus_gfs_operation_t              op,
    globus_result_t                     result,
    globus_byte_t *                     buffer,
    globus_size_t                       nbytes,
    void *                              user_arg)
{
    GlobusGFSName(globus_l_gfs_hdfs_read_from_storage_cb);
    globus_l_gfs_hdfs_handle_t *      hdfs_handle;
 
    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;

    hdfs_handle->outstanding--;
    globus_free(buffer);
    globus_l_gfs_hdfs_read_from_storage(hdfs_handle);
}


static
void
globus_l_gfs_hdfs_read_from_storage(
    globus_l_gfs_hdfs_handle_t *      hdfs_handle)
{
    globus_byte_t *                     buffer;
    globus_size_t                       nbytes;
    globus_size_t                       read_length;
    globus_result_t                     rc;

    GlobusGFSName(globus_l_gfs_hdfs_read_from_storage);

    globus_mutex_lock(&hdfs_handle->mutex);
    while (hdfs_handle->outstanding < hdfs_handle->optimal_count &&
           ! hdfs_handle->done) 
    {
        buffer = globus_malloc(hdfs_handle->block_size);
        if (buffer == NULL)
        {
            rc = GlobusGFSErrorGeneric("fail to allocate buffer");
            globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            return;
        }
        /* block_length == -1 indicates transferring data to eof */
        if (hdfs_handle->block_length < 0 ||   
            hdfs_handle->block_length > hdfs_handle->block_size)
        {
            read_length = hdfs_handle->block_size;
        }
        else
        {
            read_length = hdfs_handle->block_length;
        }
 
        nbytes = hdfsRead(hdfs_handle->fs, hdfs_handle->fd, buffer, read_length);
        if (nbytes == 0)    /* eof */
        {
            hdfs_handle->done = GLOBUS_TRUE;
            sprintf(err_msg,"send %d blocks of size %d bytes\n",
                            local_io_count,local_io_block_size);
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
            local_io_count = 0;
            local_io_block_size = 0;
        }
        else
        {
            if (nbytes != local_io_block_size)
            {
                 if (local_io_block_size != 0)
                 {
                      sprintf(err_msg,"send %d blocks of size %d bytes\n",
                                      local_io_count,local_io_block_size);
                      globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
                 }
                 local_io_block_size = nbytes;
                 local_io_count=1;
            }
            else
            {
                 local_io_count++;
            }
        }
        if (! hdfs_handle->done) 
        {
            hdfs_handle->outstanding++;
            hdfs_handle->offset += nbytes;
            hdfs_handle->block_length -= nbytes;
            rc = globus_gridftp_server_register_write(hdfs_handle->op,
                                       buffer,
                                       nbytes,
                                       hdfs_handle->offset - nbytes,
                                       -1,
                                       globus_l_gfs_hdfs_read_from_storage_cb,
                                       hdfs_handle);
            if (rc != GLOBUS_SUCCESS)
            {
                rc = GlobusGFSErrorGeneric("globus_gridftp_server_register_write() fail");
                globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            }
        }
    }
    globus_mutex_unlock(&hdfs_handle->mutex);
    if (hdfs_handle->outstanding == 0)
    {
        hdfsCloseFile(hdfs_handle->fs, hdfs_handle->fd);
        globus_gridftp_server_finished_transfer(hdfs_handle->op, 
                                                GLOBUS_SUCCESS);
    }
    return;
}

/*************************************************************************
 *  send
 *  ----
 *  This interface function is called when the client requests to receive
 *  a file from the server.
 *
 *  To send a file to the client the following functions will be used in roughly
 *  the presented order.  They are doced in more detail with the
 *  gridftp server documentation.
 *
 *      globus_gridftp_server_begin_transfer();
 *      globus_gridftp_server_register_write();
 *      globus_gridftp_server_finished_transfer();
 *
 ************************************************************************/
static
void
globus_l_gfs_hdfs_send(
    globus_gfs_operation_t              op,
    globus_gfs_transfer_info_t *        transfer_info,
    void *                              user_arg)
{
    globus_l_gfs_hdfs_handle_t *       hdfs_handle;
    GlobusGFSName(globus_l_gfs_hdfs_send);
    globus_result_t                     rc;

    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;
    hdfs_handle->pathname = transfer_info->pathname;

    hdfs_handle->op = op;
    hdfs_handle->outstanding = 0;
    hdfs_handle->done = GLOBUS_FALSE;
    globus_gridftp_server_get_block_size(op, &hdfs_handle->block_size);

    globus_gridftp_server_get_read_range(hdfs_handle->op,
                                         &hdfs_handle->offset,
                                         &hdfs_handle->block_length);



    globus_gridftp_server_begin_transfer(hdfs_handle->op, 0, hdfs_handle);
    hdfs_handle->fd = hdfsOpenFile(hdfs_handle->fs, hdfs_handle->pathname, O_RDONLY, 0, 1, 0);
    if (hdfs_handle->fd == NULL)
    {
        rc = GlobusGFSErrorGeneric("open() fail");
        globus_gridftp_server_finished_transfer(op, rc);
    }

    if (! strcmp(hdfs_handle->pathname,"/dev/zero"))
    {
    }
    else 
    {
        if (hdfsSeek(hdfs_handle->fs, hdfs_handle->fd, hdfs_handle->offset) == -1) {
            rc = GlobusGFSErrorGeneric("seek() fail");
            globus_gridftp_server_finished_transfer(op, rc);
        }
    }

    globus_gridftp_server_get_optimal_concurrency(hdfs_handle->op,
                                                  &hdfs_handle->optimal_count);
    globus_l_gfs_hdfs_read_from_storage(hdfs_handle);
    return;
}

static
int
globus_l_gfs_hdfs_activate(void);

static
int
globus_l_gfs_hdfs_deactivate(void);

/*
 *  no need to change this
 */
static globus_gfs_storage_iface_t       globus_l_gfs_hdfs_dsi_iface = 
{
    GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING | GLOBUS_GFS_DSI_DESCRIPTOR_SENDER,
    globus_l_gfs_hdfs_start,
    globus_l_gfs_hdfs_destroy,
    NULL, /* list */
    globus_l_gfs_hdfs_send,
    globus_l_gfs_hdfs_recv,
    NULL, /* trev */
    NULL, /* active */
    NULL, /* passive */
    NULL, /* data destroy */
    globus_l_gfs_hdfs_command, 
    globus_l_gfs_hdfs_stat,
    NULL,
    NULL
};

/*
 *  no need to change this
 */
GlobusExtensionDefineModule(globus_gridftp_server_hdfs) =
{
    "globus_gridftp_server_hdfs",
    globus_l_gfs_hdfs_activate,
    globus_l_gfs_hdfs_deactivate,
    NULL,
    NULL,
    &local_version
};

/*
 *  no need to change this
 */
static
int
globus_l_gfs_hdfs_activate(void)
{
    globus_extension_registry_add(
        GLOBUS_GFS_DSI_REGISTRY,
        "hdfs",
        GlobusExtensionMyModule(globus_gridftp_server_hdfs),
        &globus_l_gfs_hdfs_dsi_iface);
    
    return 0;
}

/*
 *  no need to change this
 */
static
int
globus_l_gfs_hdfs_deactivate(void)
{
    globus_extension_registry_remove(
        GLOBUS_GFS_DSI_REGISTRY, "hdfs");

    return 0;
}