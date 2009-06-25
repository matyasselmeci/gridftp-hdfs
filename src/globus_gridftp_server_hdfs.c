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
#include <syslog.h>
#include <hdfs.h>
#include <stdio.h>
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/mman.h>

static const int default_id = 00;

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
    globus_result_t                     done_status; // The status of the finished transfer.
    globus_gfs_operation_t              op;
    globus_byte_t *                     buffer;
    globus_off_t *                      offsets; // The offset of each buffer.
    globus_size_t *                     nbytes; // The number of bytes in each buffer.
    short *                             used;
    int                                 optimal_count;
    int                                 max_buffer_count;
    int                                 max_file_buffer_count;
    int                                 buffer_count; // Number of buffers we currently maintain in memory waiting to be written to HDFS.
    int                                 outstanding;
    globus_mutex_t                      mutex;
    int                                 port;
    char *                              host;
    char *                              mount_point;
    int                                 mount_point_len;
    int                                 replicas;
    char *                              username;
    char *                              tmp_file_pattern;
    int                                 tmpfilefd;
    int                                 using_file_buffer;
    char *                              syslog_host; // The host to send syslog message to.
    char *                              remote_host; // The remote host connecting to us.
    char *                              local_host;  // Our local hostname.
    char                                syslog_msg[256];  // Message printed out to syslog.
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
 *  function calls associated with this session. int                                 port;
    char *                              host;
    int                                 replicas; And an oppertunity to
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

    int max_buffer_count = 200;
    int max_file_buffer_count = 1500;
    int load_limit = 20;
    int replicas;
    int port;

    hdfs_handle = (globus_l_gfs_hdfs_handle_t *)
        globus_malloc(sizeof(globus_l_gfs_hdfs_handle_t));

    memset(&finished_info, '\0', sizeof(globus_gfs_finished_info_t));
    finished_info.type = GLOBUS_GFS_OP_SESSION_START;
    finished_info.result = GLOBUS_SUCCESS;
    finished_info.info.session.session_arg = hdfs_handle;
    finished_info.info.session.username = session_info->username;
    finished_info.info.session.home_dir = "/";

    // Copy the username from the session_info to the HDFS handle.
    size_t strlength = strlen(session_info->username)+1;
    strlength = strlength < 256 ? strlength  : 256;
    hdfs_handle->username = globus_malloc(sizeof(char)*strlength);
    if (hdfs_handle->username == NULL) {
        finished_info.result = GLOBUS_FAILURE;
        globus_gridftp_server_operation_finished(
            op, GLOBUS_FAILURE, &finished_info);
        return;
    }
    strncpy(hdfs_handle->username, session_info->username, strlength);

    // Pull configuration from environment.
    hdfs_handle->replicas = 3;
    hdfs_handle->host = "hadoop-name";
    hdfs_handle->mount_point = "/mnt/hadoop";
    hdfs_handle->port = 9000;
    char * replicas_char = getenv("VDT_GRIDFTP_HDFS_REPLICAS");
    char * namenode = getenv("VDT_GRIDFTP_HDFS_NAMENODE");
    char * port_char = getenv("VDT_GRIDFTP_HDFS_PORT");
    char * mount_point_char = getenv("VDT_GRIDFTP_HDFS_MOUNT_POINT");
    char * load_limit_char = getenv("VDT_GRIDFTP_LOAD_LIMIT");

    // Pull syslog configuration from environment.
    char * syslog_host_char = getenv("VDT_GRIDFTP_SYSLOG");
    if (syslog_host_char == NULL) {
        hdfs_handle->syslog_host = NULL;
        hdfs_handle->local_host = NULL;
    } else {
        hdfs_handle->syslog_host = syslog_host_char;
        hdfs_handle->local_host = globus_malloc(sizeof(char)*256);
        memset(hdfs_handle->local_host, '\0', sizeof(char)*256);
        if (gethostname(hdfs_handle->local_host, 255) != 0) {
            sprintf(hdfs_handle->local_host, "UNKNOWN");
        }
        hdfs_handle->remote_host = session_info->host_id;
        openlog("GRIDFTP", 0, LOG_LOCAL2);
        sprintf(hdfs_handle->syslog_msg, "%s %s %%s %%i", hdfs_handle->local_host, hdfs_handle->remote_host);
    }

    // Determine the maximum number of buffers; default to 200.
    char * max_buffer_char = getenv("VDT_GRIDFTP_BUFFER_COUNT");
    if (max_buffer_char != NULL) {
        max_buffer_count = atoi(max_buffer_char);
        if ((max_buffer_count < 5)  || (max_buffer_count > 1000))
            max_buffer_count = 200;
    }
    hdfs_handle->max_buffer_count = max_buffer_count;
    sprintf(err_msg, "Max memory buffer count: %i.\n", hdfs_handle->max_buffer_count);
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);

    char * max_file_buffer_char = getenv("VDT_GRIDFTP_FILE_BUFFER_COUNT");
    if (max_file_buffer_char != NULL) {
        max_file_buffer_count = atoi(max_file_buffer_char);
        if ((max_file_buffer_count < max_buffer_count)  || (max_buffer_count > 50000))
            max_file_buffer_count = 3*max_buffer_count;
    }
    hdfs_handle->max_file_buffer_count = max_file_buffer_count;
    sprintf(err_msg, "Max file buffer count: %i.\n", hdfs_handle->max_file_buffer_count);
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);

    if (load_limit_char != NULL) {
        load_limit = atoi(load_limit_char);
        if (load_limit < 1)
            load_limit = 20;
    }

    if (mount_point_char != NULL) {
        hdfs_handle->mount_point = mount_point_char;
    }
    hdfs_handle->mount_point_len = strlen(hdfs_handle->mount_point);

    if (replicas_char != NULL) {
        replicas = atoi(replicas_char);
        if ((replicas > 1) && (replicas < 20))
            hdfs_handle->replicas = replicas;
    }
    if (namenode != NULL)
        hdfs_handle->host = namenode;
    if (port_char != NULL) {
        port = atoi(port_char);
        if ((port >= 1) && (port <= 65535))
            hdfs_handle->port = port;
    }

    hdfs_handle->using_file_buffer = 0;

    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Checking current load on the server.\n");
    // Stall stall stall!
    int fd = open("/proc/loadavg", O_RDONLY);
    int bufsize = 256, nbytes=-1;
    char buf[bufsize];
    char * buf_ptr;
    char * token;
    double load;
    int ctr = 0;
    while (fd >= 0) {
        if (ctr == 120)
            break;
        ctr += 1;
        nbytes = read(fd, buf, bufsize);
        if (nbytes < 0)
            break;
        buf[nbytes-1] = '\0';
        buf_ptr = buf;
        token = strsep(&buf_ptr, " ");
        load = strtod(token, NULL);
        sprintf(err_msg, "Detected system load %.2f.\n", load);
        globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, err_msg);
        if ((load >= load_limit) && (load < 1000)) {
            sprintf(err_msg, "Preventing gridftp transfer startup due to system load of %.2f.\n", load);
            globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP,err_msg);
            sleep(5);
        } else {
            break;
        }
        close(fd);
        fd = open("/proc/loadavg", O_RDONLY);
    }

    sprintf(err_msg, "Start gridftp server; hadoop nameserver %s, port %i, replicas %i.\n", hdfs_handle->host, hdfs_handle->port, hdfs_handle->replicas);
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);

    hdfs_handle->fs = hdfsConnect("default", 0);
    if (!hdfs_handle->fs) {
        finished_info.result = GLOBUS_FAILURE;
        globus_gridftp_server_operation_finished(
            op, GLOBUS_FAILURE, &finished_info);
        return;
    }

    hdfs_handle->tmp_file_pattern = (char *)NULL;

    globus_gridftp_server_operation_finished(
        op, GLOBUS_SUCCESS, &finished_info);
}

/*************************************************************************
 *  remove_file_buffer
 *  -------
 *  This is called when cleaning up a file buffer. The file on disk is removed and
 *  the internal memory for storing the filename is freed.
 ************************************************************************/
static void
remove_file_buffer(globus_l_gfs_hdfs_handle_t * hdfs_handle) {
    if (hdfs_handle->tmp_file_pattern) {
	sprintf(err_msg, "Removing file buffer %s.\n", hdfs_handle->tmp_file_pattern);
	globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
        unlink(hdfs_handle->tmp_file_pattern);
        globus_free(hdfs_handle->tmp_file_pattern);
	hdfs_handle->tmp_file_pattern = (char *)NULL;
    }
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
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Destroying the HDFS connection.\n");
    if (hdfs_handle->fs)
        hdfsDisconnect(hdfs_handle->fs);
    if (hdfs_handle->username)
        globus_free(hdfs_handle->username);
    if (hdfs_handle->local_host)
        globus_free(hdfs_handle->local_host);
    remove_file_buffer(hdfs_handle);
    globus_free(hdfs_handle);
    closelog();
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

    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;
    PathName=stat_info->pathname;
    while (PathName[0] == '/' && PathName[1] == '/')
    {
        PathName++;
    }
    if (strncmp(PathName, hdfs_handle->mount_point, hdfs_handle->mount_point_len)==0) {
        PathName += hdfs_handle->mount_point_len;
    }
    while (PathName[0] == '/' && PathName[1] == '/')
    {   
        PathName++;
    }

    sprintf(err_msg, "Going to do stat on file %s.\n", PathName);
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
 

    hdfsFileInfo * fileInfo = NULL;

    /* lstat is the same as stat when not operating on a link */
    if((fileInfo = hdfsGetPathInfo(hdfs_handle->fs, PathName)) == NULL)
    {
        result = GlobusGFSErrorSystemError("stat", errno);
        goto error_stat1;
    }
    sprintf(err_msg, "Finished HDFS stat operation.\n");
    globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, err_msg);

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
     //printf("Globus HDFS command.\n");
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

/*
// Not used for now
static
void
globus_l_gfs_hdfs_trev(
    globus_gfs_event_info_t *           event_info,
    void *                              user_arg
)
{

		  globus_l_gfs_hdfs_handle_t *       hdfs_handle;
		  GlobusGFSName(globus_l_gfs_hdfs_trev);

		  hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;
		  globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Recieved a transfer event.\n");

		  switch (event_info->type) {
					 case GLOBUS_GFS_EVENT_TRANSFER_ABORT:
								globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Got an abort request to the HDFS client.\n");
								break;
					 default:
								printf("Got some other transfer event.\n");
		  }
}
*/

/* receive file from client */

static
void
globus_l_gfs_hdfs_write_to_storage(
					 globus_l_gfs_hdfs_handle_t *      hdfs_handle);


/**
 * Scan through all the buffers we own, then write out all the consecutive ones to HDFS.
 */
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
		  // Loop through all our buffers; loop again if we write something.
		  while (wrote_something == 1) {
					 wrote_something=0;
					 // For each of our buffers.
					 for (i=0; i<cnt; i++) {
								if (hdfs_handle->used[i] == 1 && offsets[i] == hdfs_handle->offset) {
										  //printf("Flushing %d bytes at offset %d from buffer %d.\n", nbytes[i], hdfs_handle->offset, i);
										if (hdfs_handle->syslog_host != NULL)
										  syslog(LOG_INFO, hdfs_handle->syslog_msg, "WRITE", nbytes[i]);
										  bytes_written = hdfsWrite(hdfs_handle->fs, hdfs_handle->fd, hdfs_handle->buffer+i*hdfs_handle->block_size, nbytes[i]*sizeof(globus_byte_t));
										  if (bytes_written > 0)
													 wrote_something = 1;
										  if (bytes_written != nbytes[i]) {
													 rc = GlobusGFSErrorSystemError("Write into HDFS failed", errno);
													 hdfs_handle->done = GLOBUS_TRUE;
													 return rc;
										  }
										  hdfs_handle->used[i] = 0;
										  hdfs_handle->offset += bytes_written;
								}
					 }
		  }
		  //if (hdfs_handle->buffer_count > 10) {
		  //    printf("Waiting on buffer %d\n", hdfs_handle->offset);
		  //}
		  return rc;
}

/**
 *  Decide whether we should use a file buffer based on the current
 *  memory usage.
 *  Returns 1 if we should use a file buffer.
 *  Else, returns 0.
 */
int use_file_buffer(globus_l_gfs_hdfs_handle_t * hdfs_handle) {
        int buffer_count = hdfs_handle->buffer_count;
 
		  if (buffer_count >= hdfs_handle->max_buffer_count-1) {
            return 1;
		  }
        if ((hdfs_handle->using_file_buffer == 1) && (buffer_count > hdfs_handle->max_buffer_count/2))
            return 1;
        return 0;
}

/**
 *  Store the current output to a buffer.
 */
static
globus_result_t globus_l_gfs_hdfs_store_buffer(globus_l_gfs_hdfs_handle_t * hdfs_handle, globus_byte_t* buffer, globus_off_t offset, globus_size_t nbytes) {
		  GlobusGFSName(globus_l_gfs_hdfs_store_buffer);
		  globus_result_t rc = GLOBUS_SUCCESS;
		  int i, cnt = hdfs_handle->buffer_count;
		  short wrote_something = 0;
		  if (hdfs_handle == NULL) {
					 rc = GlobusGFSErrorGeneric("Storing buffer for un-allocated transfer");
					 return rc;
		  }

        // Determine the type of buffer to use; allocate or transfer buffers as necessary
        int use_buffer = use_file_buffer(hdfs_handle);
        if ((use_buffer == 1) && (hdfs_handle->using_file_buffer == 0)) {
            // Turn on file buffering, copy data from the current memory buffer.
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Switching from memory buffer to file buffer.\n");

            char *tmpdir=getenv("TMPDIR");
            if (tmpdir == NULL) {
                tmpdir = "/tmp";
            }
            hdfs_handle->tmp_file_pattern = globus_malloc(sizeof(char) * (strlen(tmpdir) + 32));
            sprintf(hdfs_handle->tmp_file_pattern, "%s/gridftp-hdfs-buffer-XXXXXX", tmpdir);

            hdfs_handle->tmpfilefd = mkstemp(hdfs_handle->tmp_file_pattern);
            int filedes = hdfs_handle->tmpfilefd;
            if (filedes == -1) {
                globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Failed to determine file descriptor of temporary file.\n");
                rc = GlobusGFSErrorGeneric("Failed to determine file descriptor of temporary file.");
                return rc;
            }
            sprintf(err_msg, "Created file buffer %s.\n", hdfs_handle->tmp_file_pattern);
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
            char * tmp_write = globus_calloc(hdfs_handle->block_size, sizeof(globus_byte_t));
            for (i=0; i<cnt; i++)
                write(filedes, tmp_write, sizeof(globus_byte_t)*hdfs_handle->block_size);
            globus_free(tmp_write);
            globus_byte_t * file_buffer = mmap(0, hdfs_handle->block_size*hdfs_handle->max_file_buffer_count*sizeof(globus_byte_t), PROT_READ | PROT_WRITE, MAP_PRIVATE, filedes, 0);
            memcpy(file_buffer, hdfs_handle->buffer, cnt*hdfs_handle->block_size*sizeof(globus_byte_t));
            globus_free(hdfs_handle->buffer);
            hdfs_handle->buffer = file_buffer;
            hdfs_handle->using_file_buffer = 1;
        } else if (use_buffer == 1) {
            // Do nothing.  Continue to use the file buffer for now.
        } else if (hdfs_handle->using_file_buffer == 1 && cnt < hdfs_handle->max_buffer_count) {
            // Turn off file buffering; copy data to a new memory buffer
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Switching from file buffer to memory buffer.\n");
            globus_byte_t * tmp_buffer = globus_malloc(sizeof(globus_byte_t)*hdfs_handle->block_size*cnt);
            if (tmp_buffer == NULL) {
                rc = GlobusGFSErrorGeneric("Memory allocation error.");
                globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Memory allocation error.");
                return rc;
            }
            memcpy(tmp_buffer, hdfs_handle->buffer, cnt*hdfs_handle->block_size*sizeof(globus_byte_t));
            munmap(hdfs_handle->buffer, hdfs_handle->block_size*hdfs_handle->buffer_count*sizeof(globus_byte_t));
            hdfs_handle->using_file_buffer = 0;
            close(hdfs_handle->tmpfilefd);
	    remove_file_buffer(hdfs_handle);
            hdfs_handle->buffer = tmp_buffer;
        } else {
            // Do nothing.  Continue to use the file buffer for now.
        }

        // Search for a free space in our buffer, and then actually make the copy.
		  for (i = 0; i<cnt; i++) {
					 if (hdfs_handle->used[i] == 0) {
								sprintf(err_msg, "Stored some bytes in buffer %d; offset %lu.\n", i, offset);
								globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, err_msg);
            hdfs_handle->nbytes[i] = nbytes;
            hdfs_handle->offsets[i] = offset;
            hdfs_handle->used[i] = 1;
            wrote_something=1;
            memcpy(hdfs_handle->buffer+i*hdfs_handle->block_size, buffer, nbytes*sizeof(globus_byte_t));
            break;
        }
    }

    // Check to see how many unused buffers we have;
    i = cnt;
    while (i>0) {
        i--;
        if (hdfs_handle->used[i] == 1) {
            break;
        }
    }
    i++;
    sprintf(err_msg, "There are %i extra buffers.\n", cnt-i);
    globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, err_msg);
    // If there are more than 10 unused buffers, deallocate.
    if (cnt - i > 10) {
        sprintf(err_msg, "About to deallocate %i buffers; %i will be left.\n", cnt-i, i);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
        hdfs_handle->buffer_count = i;
        hdfs_handle->nbytes = globus_realloc(hdfs_handle->nbytes, hdfs_handle->buffer_count*sizeof(globus_size_t));
        hdfs_handle->offsets = globus_realloc(hdfs_handle->offsets, hdfs_handle->buffer_count*sizeof(globus_off_t));
        hdfs_handle->used = globus_realloc(hdfs_handle->used, hdfs_handle->buffer_count*sizeof(short));
        if (hdfs_handle->using_file_buffer == 0)
            hdfs_handle->buffer = globus_realloc(hdfs_handle->buffer, hdfs_handle->buffer_count*hdfs_handle->block_size*sizeof(globus_byte_t));
        else {
            // Truncate the file holding our backing data (note we don't resize the mmap).
            ftruncate(hdfs_handle->tmpfilefd, hdfs_handle->buffer_count*hdfs_handle->block_size*sizeof(globus_byte_t));
            lseek(hdfs_handle->tmpfilefd, 0, SEEK_END);
        }
        if (hdfs_handle->buffer == NULL || hdfs_handle->nbytes==NULL || hdfs_handle->offsets==NULL || hdfs_handle->used==NULL) {
            rc = GlobusGFSErrorGeneric("Memory allocation error.");
            globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Memory allocation error.");
            globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            return rc;
        }
    }

    // If wrote_something=0, then we have filled up all our buffers; allocate a new one.
    if (wrote_something == 0) {
        hdfs_handle->buffer_count += 1;
        sprintf(err_msg, "Initializing buffer number %d.\n", hdfs_handle->buffer_count);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
        // Refuse to allocate more than the max.
        if ((hdfs_handle->using_file_buffer == 0) && (hdfs_handle->buffer_count == hdfs_handle->max_buffer_count)) {
            // Out of memory buffers; we really shouldn't hit this code anymore.
            char * hostname = globus_malloc(sizeof(char)*256);
            memset(hostname, '\0', sizeof(char)*256);
            if (gethostname(hostname, 255) != 0) {
                sprintf(hostname, "UNKNOWN");
            }
            sprintf(err_msg, "Allocated all %i memory buffers on server %s; aborting transfer.", hdfs_handle->max_buffer_count, hostname);
            globus_free(hostname);
            rc = GlobusGFSErrorGeneric(err_msg);
            globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Failed to store data into HDFS buffer.\n");
        } else if ((hdfs_handle->using_file_buffer == 1) && (hdfs_handle->buffer_count == hdfs_handle->max_file_buffer_count)) {
            // Out of file buffers.
            char * hostname = globus_malloc(sizeof(char)*256);
            memset(hostname, '\0', sizeof(char)*256);
            if (gethostname(hostname, 255) != 0) {
                sprintf(hostname, "UNKNOWN");
            }
            sprintf(err_msg, "Allocated all %i file-backed buffers on server %s; aborting transfer.", hdfs_handle->max_file_buffer_count, hostname);
            globus_free(hostname);
            rc = GlobusGFSErrorGeneric(err_msg);
            globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Failed to store data into HDFS buffer.\n");
        } else {
            // Increase the size of all our buffers which track memory usage
            hdfs_handle->nbytes = globus_realloc(hdfs_handle->nbytes, hdfs_handle->buffer_count*sizeof(globus_size_t));
            hdfs_handle->offsets = globus_realloc(hdfs_handle->offsets, hdfs_handle->buffer_count*sizeof(globus_off_t));
            hdfs_handle->used = globus_realloc(hdfs_handle->used, hdfs_handle->buffer_count*sizeof(short));
            hdfs_handle->used[hdfs_handle->buffer_count-1] = 1;
            // Only reallocate the physical buffer if we're using a memory buffer, otherwise we screw up our mmap
            if (hdfs_handle->using_file_buffer == 0) {
                hdfs_handle->buffer = globus_realloc(hdfs_handle->buffer, hdfs_handle->buffer_count*hdfs_handle->block_size*sizeof(globus_byte_t));
            } else {
                // This not only extends the size of our file, but we extend it with the desired buffer data.
                lseek(hdfs_handle->tmpfilefd, (hdfs_handle->buffer_count-1)*hdfs_handle->block_size, SEEK_SET);
                write(hdfs_handle->tmpfilefd, buffer, nbytes*sizeof(globus_byte_t));
                // If our buffer was too small, 
                if (nbytes < hdfs_handle->block_size) {
                    int addl_size = hdfs_handle->block_size-nbytes;
                    char * tmp_write = globus_calloc(addl_size, sizeof(globus_byte_t));
                    write(hdfs_handle->tmpfilefd, tmp_write, sizeof(globus_byte_t)*addl_size);
                    globus_free(tmp_write);
                }
                //hdfs_handle->buffer = mmap(hdfs_handle->buffer, hdfs_handle->block_size*hdfs_handle->max_file_buffer_count*sizeof(globus_byte_t), PROT_READ | PROT_WRITE, MAP_PRIVATE, hdfs_handle->tmpfilefd, 0);
            }
            if (hdfs_handle->buffer == NULL || hdfs_handle->nbytes==NULL || hdfs_handle->offsets==NULL || hdfs_handle->used==NULL) {  
                rc = GlobusGFSErrorGeneric("Memory allocation error.");
                globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Memory allocation error.");
                globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            }
            // In the case where we have file buffers, we already wrote the contents of buffer previously.
            if (hdfs_handle->using_file_buffer == 0) {
                memcpy(hdfs_handle->buffer+(hdfs_handle->buffer_count-1)*hdfs_handle->block_size, buffer, nbytes*sizeof(globus_byte_t));
            }
            hdfs_handle->nbytes[hdfs_handle->buffer_count-1] = nbytes;
            hdfs_handle->offsets[hdfs_handle->buffer_count-1] = offset;
        }
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
    if (hdfs_handle->done && hdfs_handle->done_status != GLOBUS_SUCCESS) {
        //globus_gridftp_server_finished_transfer(op, hdfs_handle->done_status);
        //return;
        rc = hdfs_handle->done_status;
        goto cleanup;
    }
    if (result != GLOBUS_SUCCESS)
    {
        //printf("call back fail.\n");
        rc = GlobusGFSErrorGeneric("call back fail");
        hdfs_handle->done = GLOBUS_TRUE;
    }
    else if (eof)
    {
        hdfs_handle->done = GLOBUS_TRUE;
    }
    if (nbytes > 0)
    {
        // First, see if we can dump this block immediately.
        if (offset == hdfs_handle->offset) {
            sprintf(err_msg, "Dumping this block immediately.\n");
            globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, err_msg);
            if (hdfs_handle->syslog_host != NULL)
                syslog(LOG_INFO, hdfs_handle->syslog_msg, "WRITE", nbytes);
            globus_size_t bytes_written = hdfsWrite(hdfs_handle->fs, hdfs_handle->fd, buffer, nbytes);
            if (bytes_written != nbytes) {
                rc = GlobusGFSErrorSystemError("Write into HDFS failed", errno);
                hdfs_handle->done = GLOBUS_TRUE;
            } else {
                hdfs_handle->offset += bytes_written;
                // Try to write out as many buffers as we can to HDFS.
                rc = globus_l_gfs_hdfs_dump_buffers(hdfs_handle);
                if (rc != GLOBUS_SUCCESS) {
                    hdfs_handle->done = GLOBUS_TRUE;
                }
                globus_gridftp_server_update_bytes_written(op, offset, nbytes);
            }
        } else {
            // Try to store the buffer into memory.
            rc = globus_l_gfs_hdfs_store_buffer(hdfs_handle, buffer, offset, nbytes);
            if (rc != GLOBUS_SUCCESS) {
                //printf("Store failed.\n");
                hdfs_handle->done = GLOBUS_TRUE;
            } else {
                // Try to write out as many buffers as we can to HDFS.
                rc = globus_l_gfs_hdfs_dump_buffers(hdfs_handle);
                if (rc != GLOBUS_SUCCESS) {
                    hdfs_handle->done = GLOBUS_TRUE;
                }
                globus_gridftp_server_update_bytes_written(op, offset, nbytes);
            }
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

    cleanup:

    hdfs_handle->outstanding--;
    if (! hdfs_handle->done)
    {
        // Ask for more transfers!
        globus_l_gfs_hdfs_write_to_storage(hdfs_handle);
    } else if (hdfs_handle->outstanding == 0) {
        if (hdfs_handle->using_file_buffer == 0)        
            globus_free(hdfs_handle->buffer);
        else {
            munmap(hdfs_handle->buffer, hdfs_handle->block_size*hdfs_handle->buffer_count*sizeof(globus_byte_t));
            hdfs_handle->using_file_buffer = 0;
            close(hdfs_handle->tmpfilefd);
        }
        globus_free(hdfs_handle->used);
        globus_free(hdfs_handle->nbytes);
        globus_free(hdfs_handle->offsets);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Trying to close file in HDFS; zero outstanding blocks.\n");
        if (hdfsCloseFile(hdfs_handle->fs, hdfs_handle->fd) == -1) 
        {
             if (rc == GLOBUS_SUCCESS)
               rc = GlobusGFSErrorGeneric("Failed to close file in HDFS.");
             globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Failed to close file in HDFS.\n");
        }
        sprintf(err_msg,"receive %d blocks of size %d bytes\n",
                        local_io_count,local_io_block_size);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
        local_io_count = 0;
        local_io_block_size = 0;

        globus_gridftp_server_finished_transfer(op, rc);
    } else if (rc != GLOBUS_SUCCESS) {  // Done is set, but we have outstanding I/O = failed somewhere.
        // Don't close the file because the other transfers will want to finish up.
        sprintf(err_msg, "We failed to finish the transfer, but there are %i outstanding writes left over.\n", hdfs_handle->outstanding);
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO,err_msg);
        hdfs_handle->done_status = rc;
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
    sprintf(err_msg, "Globus write_to_storage; outstanding %d, optimal %d.\n", hdfs_handle->outstanding, hdfs_handle->optimal_count);
    globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, err_msg);
    while (hdfs_handle->outstanding < hdfs_handle->optimal_count) 
    {
        buffer = globus_malloc(hdfs_handle->block_size);
        if (buffer == NULL)
        {
            rc = GlobusGFSErrorMemory("Fail to allocate buffer for HDFS data.");
            globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            return;
        }
        //globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, "About to register read.\n");
        rc = globus_gridftp_server_register_read(hdfs_handle->op,
                                       buffer,
                                       hdfs_handle->block_size,
                                       globus_l_gfs_hdfs_write_to_storage_cb,
                                       hdfs_handle);
        if (rc != GLOBUS_SUCCESS)
        {
            rc = GlobusGFSErrorGeneric("globus_gridftp_server_register_read() fail");
            globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "globus_gridftp_server_register_read() fail\n");
            globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
            return;
        } else {
            //globus_gfs_log_message(GLOBUS_GFS_LOG_DUMP, "Finished read registration successfully.\n");
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
    globus_result_t                     rc = GLOBUS_SUCCESS; 

    GlobusGFSName(globus_l_gfs_hdfs_recv);

    hdfs_handle = (globus_l_gfs_hdfs_handle_t *) user_arg;

    hdfs_handle->pathname = transfer_info->pathname;
    while (hdfs_handle->pathname[0] == '/' && hdfs_handle->pathname[1] == '/')
    {
        hdfs_handle->pathname++;
    }
    if (strncmp(hdfs_handle->pathname, hdfs_handle->mount_point, hdfs_handle->mount_point_len) == 0) {
        hdfs_handle->pathname += hdfs_handle->mount_point_len;
    }
    while (hdfs_handle->pathname[0] == '/' && hdfs_handle->pathname[1] == '/')
    {
        hdfs_handle->pathname++;
    }

    sprintf(err_msg, "We are going to open file %s.\n", hdfs_handle->pathname);
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
    hdfs_handle->op = op;
    hdfs_handle->outstanding = 0;
    hdfs_handle->done = GLOBUS_FALSE;
    hdfs_handle->done_status = GLOBUS_SUCCESS;
    globus_gridftp_server_get_block_size(op, &hdfs_handle->block_size); 

    globus_gridftp_server_get_write_range(hdfs_handle->op,
                                          &hdfs_handle->offset,
                                          &hdfs_handle->block_length);

    if (hdfs_handle->offset != 0) {
        globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, "Non-zero offsets are not supported.");
        rc = GlobusGFSErrorGeneric("Non-zero offsets are not supported.");
        globus_gridftp_server_finished_transfer(op, rc);
        return;
    }


    // Check to make sure file exists, then open it write-only.
    sprintf(err_msg, "Open file %s.\n", hdfs_handle->pathname);
    globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, err_msg);
    if (hdfsExists(hdfs_handle->fs, hdfs_handle->pathname) == 0)
    {
        hdfsFileInfo *fileInfo;
        if((fileInfo = hdfsGetPathInfo(hdfs_handle->fs, hdfs_handle->pathname)) == NULL)
        {
            rc = GlobusGFSErrorGeneric("File exists in HDFS, but failed to perform `stat` on it.");
            globus_gridftp_server_finished_transfer(op, rc);
            return;
        }
        if (fileInfo->mKind == kObjectKindDirectory) {
            rc = GlobusGFSErrorGeneric("Destination path is a directory; cannot overwrite.");
            globus_gridftp_server_finished_transfer(op, rc);
            return;
        }
        hdfs_handle->fd = hdfsOpenFile(hdfs_handle->fs, hdfs_handle->pathname,
            O_WRONLY, 0, 0, 0);
    }
    else
    {
        hdfs_handle->fd = hdfsOpenFile(hdfs_handle->fs, hdfs_handle->pathname,
                                 O_WRONLY, 0, 0, 0);
    }
    if (!hdfs_handle->fd)
    {
        char * hostname = globus_malloc(sizeof(char)*256);
        memset(hostname, '\0', sizeof(char)*256);
        if (gethostname(hostname, 255) != 0) {
            sprintf(hostname, "UNKNOWN");
        }
        if (errno == EINTERNAL) {
            sprintf(err_msg, "Failed to open file %s in HDFS for user %s due to an internal error in HDFS "
                "on server %s; could be a misconfiguration or bad installation at the site",
                hdfs_handle->pathname, hdfs_handle->username, hostname);
            rc = GlobusGFSErrorSystemError(err_msg, errno);
        } else if (errno == EACCES) {
            sprintf(err_msg, "Permission error in HDFS from gridftp server %s; user %s is not allowed"
                " to open the HDFS file %s", hostname,
                hdfs_handle->username, hdfs_handle->pathname);
            rc = GlobusGFSErrorSystemError(err_msg, errno);
        } else {
            sprintf(err_msg, "Failed to open file %s in HDFS for user %s on server %s; unknown error from HDFS",
                hdfs_handle->pathname, hdfs_handle->username, hostname);
            rc = GlobusGFSErrorSystemError(err_msg, errno);
        }
        globus_gridftp_server_finished_transfer(op, rc);
        globus_free(hostname);
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
        rc = GlobusGFSErrorMemory("Memory allocation error.");
        globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
        sprintf(err_msg, "Memory allocation error.\n");
        globus_gfs_log_message(GLOBUS_GFS_LOG_ERR, err_msg);
        return;
    }

    globus_gridftp_server_begin_transfer(hdfs_handle->op, 0, hdfs_handle);
    globus_mutex_lock(&hdfs_handle->mutex);
    if (rc == GLOBUS_SUCCESS)
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
            rc = GlobusGFSErrorMemory("Fail to allocate buffer for HDFS.");
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

        if (hdfs_handle->syslog_host != NULL)
            syslog(LOG_INFO, hdfs_handle->syslog_msg, "READ", read_length);
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
        globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "Trying to close file in HDFS.\n");
        if (hdfsCloseFile(hdfs_handle->fs, hdfs_handle->fd) == -1)
        {
             rc = GlobusGFSErrorGeneric("Failed to close file in HDFS.");
             globus_gridftp_server_finished_transfer(hdfs_handle->op, rc);
        } else {
        globus_gridftp_server_finished_transfer(hdfs_handle->op, 
                                                GLOBUS_SUCCESS);
        }
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
    while (hdfs_handle->pathname[0] == '/' && hdfs_handle->pathname[1] == '/')
    {
        hdfs_handle->pathname++;
    }
    if (strncmp(hdfs_handle->pathname, hdfs_handle->mount_point, hdfs_handle->mount_point_len)==0) {
        hdfs_handle->pathname += hdfs_handle->mount_point_len;
    }
    while (hdfs_handle->pathname[0] == '/' && hdfs_handle->pathname[1] == '/')
    {
        hdfs_handle->pathname++;
    }

    hdfs_handle->op = op;
    hdfs_handle->outstanding = 0;
    hdfs_handle->done = GLOBUS_FALSE;
    globus_gridftp_server_get_block_size(op, &hdfs_handle->block_size);

    globus_gridftp_server_get_read_range(hdfs_handle->op,
                                         &hdfs_handle->offset,
                                         &hdfs_handle->block_length);



    globus_gridftp_server_begin_transfer(hdfs_handle->op, 0, hdfs_handle);

    if (hdfsExists(hdfs_handle->fs, hdfs_handle->pathname) == 0)
    {
        hdfsFileInfo *fileInfo;
        int hasStat = 1;

        if((fileInfo = hdfsGetPathInfo(hdfs_handle->fs, hdfs_handle->pathname)) == NULL)
            hasStat = 0;
printf("File exists.\n");

        if (hasStat && fileInfo->mKind == kObjectKindDirectory) {
            char * hostname = globus_malloc(sizeof(char)*256);
            memset(hostname, '\0', sizeof(char)*256);
            if (gethostname(hostname, 255) != 0) {
                sprintf(hostname, "UNKNOWN");
            }
            sprintf(err_msg, "Error for user %s accessing gridftp server %s.  The file you are trying to"
                " read, %s, is a directory.", hdfs_handle->username, hostname, hdfs_handle->pathname);
            rc = GlobusGFSErrorGeneric(err_msg);
            globus_free(hostname);
            globus_gridftp_server_finished_transfer(op, rc);
            return;
        }
    } else {
        char * hostname = globus_malloc(sizeof(char)*256);
        memset(hostname, '\0', sizeof(char)*256);
        if (gethostname(hostname, 255) != 0) {
            sprintf(hostname, "UNKNOWN");
        }
        sprintf(err_msg, "Error for user %s accessing gridftp server %s.  The file you are trying to "
                "read, %s, does not exist.", hdfs_handle->username, hostname, hdfs_handle->pathname);
        rc = GlobusGFSErrorGeneric(err_msg);
        globus_free(hostname);
        globus_gridftp_server_finished_transfer(op, rc);
        return;
    }


    hdfs_handle->fd = hdfsOpenFile(hdfs_handle->fs, hdfs_handle->pathname, O_RDONLY, 0, 1, 0);
    if (!hdfs_handle->fd)
    {
        char * hostname = globus_malloc(sizeof(char)*256);
        memset(hostname, '\0', sizeof(char)*256);
        if (gethostname(hostname, 255) != 0) {
            sprintf(hostname, "UNKNOWN");
        }
        if (errno == EINTERNAL) {
            sprintf(err_msg, "Failed to open file %s in HDFS for user %s due to an internal error in HDFS "
                "on server %s; could be a misconfiguration or bad installation at the site",
                hdfs_handle->pathname, hdfs_handle->username, hostname);
            rc = GlobusGFSErrorSystemError(err_msg, errno);
        } else if (errno == EACCES) {
            sprintf(err_msg, "Permission error in HDFS from gridftp server %s; user %s is not allowed"
                " to open the HDFS file %s", hostname,
                hdfs_handle->username, hdfs_handle->pathname);
            rc = GlobusGFSErrorSystemError(err_msg, errno);
        } else if (errno == ENOENT) {
            sprintf(err_msg, "Failure for user %s on server %s; the requested file %s does not exist", hdfs_handle->username,
                hostname, hdfs_handle->pathname);
            rc = GlobusGFSErrorSystemError(err_msg, errno);
        } else {
            sprintf(err_msg, "Failed to open file %s in HDFS for user %s on server %s; unknown error from HDFS",
                hdfs_handle->pathname, hdfs_handle->username, hostname);
            rc = GlobusGFSErrorSystemError(err_msg, errno);
        }
        globus_gridftp_server_finished_transfer(op, rc);
        globus_free(hostname);
        return;
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
    NULL, /*globus_l_gfs_hdfs_trev, */ /* trev */
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