/*
    Authors:
        Pavel Březina <pbrezina@redhat.com>

    Copyright (C) 2017 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <sys/stat.h>

#include "authselect.h"
#include "common/common.h"
#include "cli/cli_tool.h"

#define CLI_ERROR(msg, ...) fprintf(stderr, gettext(msg), ## __VA_ARGS__)

#define CLI_PRINT(msg, ...) printf(gettext(msg), ## __VA_ARGS__)
#define CLI_MSG(quiet, msg, ...) do {         \
    if (!quiet) {                             \
        CLI_PRINT((msg), ## __VA_ARGS__);     \
    }                                         \
} while (0)

static size_t
list_max_length(char **list)
{
    size_t max = 0;
    size_t len;
    int i;

    for (i = 0; list[i] != NULL; i++) {
        len = strlen(list[i]);
        if (max < len) {
            max = len;
        }
    }

    return max;
}

static errno_t
parse_profile_options(struct cli_cmdline *cmdline,
                      struct poptOption *options,
                      char **_profile_id,
                      const char ***_features)
{
    char *profile_id;
    const char **features;
    bool profile_skipped;
    errno_t ret;
    int i, j;

    *_profile_id = NULL;

    ret = cli_tool_popt_ex(cmdline, options, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "PROFILE-ID", _("Profile identifier."),
                           &profile_id, true, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        return ret;
    }

    features = malloc_zero_array(const char *, cmdline->argc);
    if (features == NULL) {
        free(profile_id);
        return ENOMEM;
    }

    profile_skipped = false;
    for (i = 0, j = 0; i < cmdline->argc; i++) {
        /* Skip options. */
        if (strcmp(cmdline->argv[i], "--backup") == 0) {
            /* Skip also the next parameter which is the backup name. */
            i++;
            continue;
        }

        if (cmdline->argv[i][0] == '-') {
            continue;
        }

        /* First free option is profile name. We must skip it. For example:
         * authselect select sssd with-feature --force
         * authselect select --force sssd with-feature
         */
        if (!profile_skipped) {
            profile_skipped = true;
            continue;
        }

        features[j] = cmdline->argv[i];
        j++;
    }

    *_profile_id = profile_id;
    *_features = features;

    return EOK;
}

static errno_t
perform_backup(int quiet,
               int backup,
               const char *backup_name)
{
    char *backup_path;
    errno_t ret;

    if (!backup && backup_name == NULL) {
        return EOK;
    }

    ret = authselect_backup(backup_name, &backup_path);
    if (ret != EOK) {
        CLI_ERROR("Unable to backup current configuration!\n");
        return ret;
    }

    CLI_MSG(quiet, "Backup stored at %s\n", backup_path);
    free(backup_path);

    return EOK;
}

static errno_t activate(struct cli_cmdline *cmdline)
{
    struct authselect_profile *profile = NULL;
    const char **features = NULL;
    char *profile_id = NULL;
    char *requirements = NULL;
    char *backup_name = NULL;
    char **maps = NULL;
    int backup = 0;
    int nobackup = 0;
    int enforce = 0;
    int quiet = 0;
    errno_t ret;
    int i;

    struct poptOption options[] = {
        {"force", 'f', POPT_ARG_VAL, &enforce, 1, _("Enforce changes"), NULL },
        {NULL, 'b', POPT_ARG_VAL, &backup, 1, _("Backup system files before activating profile (generate unique name)"), NULL },
        {"backup", '\0', POPT_ARG_STRING | POPT_ARG_NONE, &backup_name, 0, _("Backup system files before activating profile"), _("NAME") },
        {"nobackup", '\0', POPT_ARG_VAL, &nobackup, 1, _("Do not backup system files when --force is set"), NULL },
        {"quiet", 'q', POPT_ARG_VAL, &quiet, 1, _("Do not print profile requirements"), NULL },
        POPT_TABLEEND
    };

    ret = parse_profile_options(cmdline, options, &profile_id, &features);
    if (ret != EOK) {
        goto done;
    }

    ret = authselect_profile(profile_id, &profile);
    if (ret != EOK) {
        ERROR("Unable to get profile information [%d]: %s",
              ret, strerror(ret));
        ret = ENOMEM;
        goto done;
    }

    requirements = authselect_profile_requirements(profile, features);
    if (requirements == NULL) {
        ERROR("Unable to read profile requirements!");
        ret = EFAULT;
        goto done;
    }

    if (backup || backup_name != NULL || (enforce && !nobackup)) {
        ret = perform_backup(quiet, 1, backup_name);
        if (ret != EOK) {
            goto done;
        }
    }

    ret = authselect_activate(profile_id, features, enforce);
    if (ret == EEXIST) {
        CLI_ERROR("\nSome unexpected changes to the configuration were "
                  "detected.\nUse --force parameter if you want to overwrite "
                  "these changes.\n");
        goto done;
    } else if (ret != EOK) {
        CLI_ERROR("Unable to activate profile [%d]: %s\n",
                  ret, strerror(ret));
        goto done;
    }

    CLI_MSG(quiet, "Profile \"%s\" was selected.\n", profile_id);

    if (maps != NULL && maps[0] != NULL) {
        CLI_MSG(quiet, "The following nsswitch maps are overwritten "
                       "by the profile:\n");
        for (i = 0; maps[i] != NULL; i++) {
            CLI_MSG(quiet, "- %s\n", maps[i]);
        }
    }

    if (requirements[0] != '\0') {
        CLI_MSG(quiet, "\n%s\n", requirements);
    }

    ret = EOK;

done:
    free(requirements);
    authselect_array_free(maps);
    authselect_profile_free(profile);
    free(features);
    free(profile_id);

    return ret;
}

static errno_t apply_changes(struct cli_cmdline *cmdline)
{
    char *backup_name = NULL;
    int backup = 0;
    errno_t ret;

    struct poptOption options[] = {
        {NULL, 'b', POPT_ARG_VAL, &backup, 1, _("Backup system files before activating profile (generate unique name)"), NULL },
        {"backup", '\0', POPT_ARG_STRING | POPT_ARG_NONE, &backup_name, 0, _("Backup system files before activating profile"), _("NAME") },
        POPT_TABLEEND
    };

    ret = cli_tool_popt(cmdline, options, CLI_TOOL_OPT_OPTIONAL, NULL, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        return ret;
    }

    ret = perform_backup(false, backup, backup_name);
    if (ret != EOK) {
        return ret;
    }

    ret = authselect_apply_changes();
    switch (ret) {
    case EOK:
        CLI_PRINT("Changes were successfully applied.\n");
        break;
    case ENOENT:
        CLI_ERROR("No existing configuration detected.\n");
        break;
    case EEXIST:
        CLI_ERROR("Some unexpected changes to the configuration were "
                  "detected. Use 'select' command instead.\n");
        break;
    default:
        CLI_ERROR("Unable to apply changes [%d]: %s\n", ret, strerror(ret));
        break;
    }

    return ret;
}

static errno_t current(struct cli_cmdline *cmdline)
{
    int raw_output = 0;
    char *profile_id;
    char **features;
    errno_t ret;
    int i;

    struct poptOption options[] = {
        {"raw", 'r', POPT_ARG_VAL, &raw_output, 1,
         _("Print command parameters instead of formatted output"), NULL },
        POPT_TABLEEND
    };

    ret = cli_tool_popt(cmdline, options, CLI_TOOL_OPT_OPTIONAL, NULL, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        return ret;
    }

    ret = authselect_current_configuration(&profile_id, &features);
    if (ret == ENOENT) {
        CLI_PRINT("No existing configuration detected.\n");
        return ret;
    } else if (ret != EOK) {
        ERROR("Unable to get current configuration [%d]: %s",
              ret, strerror(ret));
        return ret;
    }

    if (raw_output) {
        printf("%s", profile_id);
        if (features != NULL) {
            for (i = 0; features[i] != NULL; i++) {
                printf(" %s", features[i]);
            }
        }
        printf("\n");
    } else {
        CLI_PRINT("Profile ID: %s\n", profile_id);
        CLI_PRINT("Enabled features:");

        if (features == NULL || features[0] == NULL) {
            CLI_PRINT(" None\n");
        } else {
            printf("\n");
            for (i = 0; features[i] != NULL; i++) {
                printf("- %s\n", features[i]);
            }
        }
    }

    free(profile_id);
    authselect_array_free(features);

    return EOK;
}

static errno_t check(struct cli_cmdline *cmdline)
{
    bool is_valid;
    errno_t ret;

    ret = cli_tool_popt(cmdline, NULL, CLI_TOOL_OPT_OPTIONAL, NULL, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        return ret;
    }

    ret = authselect_validate_configuration(&is_valid);
    if (ret != EOK && ret != ENOENT && ret != EEXIST) {
        ERROR("Unable to test current configuration [%d]: %s",
              ret, strerror(ret));

        return ret;
    }

    if (!is_valid) {
        puts(_("Current configuration is not valid. "
               "It was probably modified outside authselect."));
        return EBADF;
    }

    switch (ret) {
    case EOK:
        puts(_("Current configuration is valid."));
        break;
    case ENOENT:
        puts(_("No configuration detected."));
        ret = ENODEV;
        break;
    case EEXIST:
        puts(_("System was not configured with authselect."));
        ret = ENOENT; /* for backwards compatibility */
        break;
    }

    /* EOK = existing configuration is valid,
     * ENODEV = no configuration detected,
     * ENOENT = non-authselet configuration is valid */
    return ret;
}

static errno_t list(struct cli_cmdline *cmdline)
{
    struct authselect_profile *profile;
    char **profiles;
    errno_t ret;
    int maxlen;
    int i;

    ret = cli_tool_popt(cmdline, NULL, CLI_TOOL_OPT_OPTIONAL, NULL, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        return ret;
    }

    profiles = authselect_list();
    if (profiles == NULL) {
        ERROR("Unable to get profile list!");
        return ENOMEM;
    }

    maxlen = list_max_length(profiles);

    for (i = 0; profiles[i] != NULL; i++) {
        ret = authselect_profile(profiles[i], &profile);
        if (ret != EOK) {
            ERROR("Unable to get profile information [%d]: %s",
                  ret, strerror(ret));
            goto done;
        }

        printf("- %-*s\t %s\n", maxlen, profiles[i],
               authselect_profile_name(profile));

        authselect_profile_free(profile);
    }

    ret = EOK;

done:
    authselect_array_free(profiles);
    return ret;
}

static errno_t list_features(struct cli_cmdline *cmdline)
{
    struct authselect_profile *profile;
    char *profile_id;
    char **features;
    errno_t ret;
    int i;

    ret = cli_tool_popt_ex(cmdline, NULL, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "PROFILE-ID", _("Profile identifier."),
                           &profile_id, true, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        goto done;
    }

    ret = authselect_profile(profile_id, &profile);
    if (ret != EOK) {
        ERROR("Unable to get profile information [%d]: %s",
              ret, strerror(ret));
        goto done;
    }

    features = authselect_profile_features(profile);
    authselect_profile_free(profile);
    if (features == NULL) {
        ERROR("Unable to get profile features [%d]: %s",
              ret, strerror(ret));
        ret = ENOMEM;
        goto done;
    }

    for (i = 0; features[i] != NULL; i++) {
        puts(features[i]);
    }

    authselect_array_free(features);

    ret = EOK;

done:
    free(profile_id);
    return ret;
}

static errno_t show(struct cli_cmdline *cmdline)
{
    struct authselect_profile *profile;
    char *profile_id;
    errno_t ret;

    ret = cli_tool_popt_ex(cmdline, NULL, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "PROFILE-ID", _("Profile identifier."),
                           &profile_id, false, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        goto done;
    }

    ret = authselect_profile(profile_id, &profile);
    if (ret != EOK) {
        ERROR("Unable to get profile information [%d]: %s",
              ret, strerror(ret));
        ret = ENOMEM;
        goto done;
    }

    puts(authselect_profile_description(profile));

    authselect_profile_free(profile);

    ret = EOK;

done:
    free(profile_id);
    return ret;
}

static errno_t requirements(struct cli_cmdline *cmdline)
{
    struct authselect_profile *profile = NULL;
    char *profile_id = NULL;
    const char **features;
    char *requirements = NULL;
    errno_t ret;

    ret = parse_profile_options(cmdline, NULL, &profile_id, &features);
    if (ret != EOK) {
        goto done;
    }

    ret = authselect_profile(profile_id, &profile);
    if (ret != EOK) {
        ERROR("Unable to get profile information [%d]: %s",
              ret, strerror(ret));
        ret = ENOMEM;
        goto done;
    }

    requirements = authselect_profile_requirements(profile, features);
    if (requirements == NULL) {
        ERROR("Unable to read profile requirements!");
        ret = EFAULT;
        goto done;
    } else if (requirements[0] == '\0') {
        puts("No requirements are specified.");
    } else {
        puts(requirements);
    }

    ret = EOK;

done:
    free(requirements);
    free(profile_id);
    authselect_profile_free(profile);

    return ret;
}

static errno_t test(struct cli_cmdline *cmdline)
{
    struct authselect_files *files;
    char *profile_id = NULL;
    const char **features;
    const char *content;
    const char *path;
    int print_all = 1;
    int print_nsswitch = 0;
    int print_systemauth = 0;
    int print_passwordauth = 0;
    int print_smartcardauth = 0;
    int print_fingerprintauth = 0;
    int print_postlogin = 0;
    int print_dconfdb = 0;
    int print_dconflock = 0;
    errno_t ret;
    int i;

    struct poptOption options[] = {
        {"all", 'a', POPT_ARG_VAL, &print_all, 1, _("Print content of all files"), NULL },
        {"nsswitch", 'n', POPT_ARG_VAL, &print_nsswitch, 1, _("Print nsswitch.conf content"), NULL },
        {"system-auth", 's', POPT_ARG_VAL, &print_systemauth, 1, _("Print system-auth content"), NULL },
        {"password-auth", 'p', POPT_ARG_VAL, &print_passwordauth, 1, _("Print password-auth content"), NULL },
        {"smartcard-auth", 'c', POPT_ARG_VAL, &print_smartcardauth, 1, _("Print smartcard-auth content"), NULL },
        {"fingerprint-auth", 'f', POPT_ARG_VAL, &print_fingerprintauth, 1, _("Print fingerprint-auth content"), NULL },
        {"postlogin", 'o', POPT_ARG_VAL, &print_postlogin, 1, _("Print postlogin content"), NULL },
        {"dconf-db", 'd', POPT_ARG_VAL, &print_dconfdb, 1, _("Print dconf database content"), NULL },
        {"dconf-lock", 'l', POPT_ARG_VAL, &print_dconflock, 1, _("Print dconf lock content"), NULL },
        POPT_TABLEEND
        };

    struct {
        const char * (*content_fn)(const struct authselect_files *);
        const char * (*path_fn)(void);
        int *enabled;
    } generated[] = {
        {authselect_files_nsswitch, authselect_path_nsswitch, &print_nsswitch},
        {authselect_files_systemauth, authselect_path_systemauth, &print_systemauth},
        {authselect_files_passwordauth, authselect_path_passwordauth, &print_passwordauth},
        {authselect_files_smartcardauth, authselect_path_smartcardauth, &print_smartcardauth},
        {authselect_files_fingerprintauth, authselect_path_fingerprintauth, &print_fingerprintauth},
        {authselect_files_postlogin, authselect_path_postlogin, &print_postlogin},
        {authselect_files_dconf_db, authselect_path_dconf_db, &print_dconfdb},
        {authselect_files_dconf_lock, authselect_path_dconf_lock, &print_dconflock},
        {NULL, NULL, NULL}
    };

    ret = parse_profile_options(cmdline, options, &profile_id, &features);
    if (ret != EOK) {
        goto done;
    }

    ret = authselect_files(profile_id, features, &files);
    if (ret != EOK) {
        ERROR("Unable to get generated content [%d]: %s", ret, strerror(ret));
        goto done;
    }

    for (i = 0; generated[i].content_fn != NULL; i++) {
        if (*generated[i].enabled == 1) {
            print_all = 0;
        }
    }

    for (i = 0; generated[i].content_fn != NULL; i++) {
        if (!print_all && *generated[i].enabled == 0) {
            continue;
        }

        path = generated[i].path_fn();
        content = generated[i].content_fn(files);

        if (content == NULL) {
            CLI_PRINT("File %s: Empty\n\n", path);
        } else {
            CLI_PRINT("File %s:\n%s\n\n", path, content);
        }
    }

done:
    free(profile_id);
    return ret;
}

static errno_t enable(struct cli_cmdline *cmdline)
{
    struct authselect_profile *profile = NULL;
    char *backup_name = NULL;
    char *requirements = NULL;
    char *profile_id = NULL;
    char *feature;
    const char *features[2];
    int backup = 0;
    int quiet = 0;
    errno_t ret;

    struct poptOption options[] = {
        {NULL, 'b', POPT_ARG_VAL, &backup, 1, _("Backup system files before activating profile (generate unique name)"), NULL },
        {"backup", '\0', POPT_ARG_STRING | POPT_ARG_NONE, &backup_name, 0, _("Backup system files before activating profile"), _("NAME") },
        {"quiet", 'q', POPT_ARG_VAL, &quiet, 1, _("Do not print profile requirements"), NULL },
        POPT_TABLEEND
    };

    ret = cli_tool_popt_ex(cmdline, options, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "FEATURE", _("Feature to enable."),
                           &feature, false, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        return ret;
    }

    features[0] = feature;
    features[1] = NULL;

    ret = authselect_current_configuration(&profile_id, NULL);
    if (ret == ENOENT) {
        CLI_PRINT("No existing configuration detected.\n");
        goto done;
    } else if (ret != EOK) {
        ERROR("Unable to get current configuration [%d]: %s",
              ret, strerror(ret));
        goto done;
    }

    ret = authselect_profile(profile_id, &profile);
    if (ret != EOK) {
        ERROR("Unable to get profile information [%d]: %s",
              ret, strerror(ret));
        ret = ENOMEM;
        goto done;
    }

    requirements = authselect_profile_requirements(profile, features);
    if (requirements == NULL) {
        ERROR("Unable to read profile requirements!");
        ret = EFAULT;
        goto done;
    }

    ret = perform_backup(quiet, backup, backup_name);
    if (ret != EOK) {
        CLI_ERROR("Unable to backup current configuration [%d]: %s\n",
                  ret, strerror(ret));
        goto done;
    }

    ret = authselect_feature_enable(feature);
    if (ret != EOK) {
        CLI_ERROR("Unable to enable feature [%d]: %s\n", ret, strerror(ret));
        goto done;
    }

    if (requirements[0] != '\0') {
        CLI_MSG(quiet, "%s\n", requirements);
    }

    ret = EOK;

done:
    free(profile_id);
    free(requirements);
    free(feature);
    authselect_profile_free(profile);

    return ret;
}

static errno_t disable(struct cli_cmdline *cmdline)
{
    int backup = 0;
    char *backup_name = NULL;
    char *feature;
    errno_t ret;

    struct poptOption options[] = {
        {NULL, 'b', POPT_ARG_VAL, &backup, 1, _("Backup system files before activating profile (generate unique name)"), NULL },
        {"backup", '\0', POPT_ARG_STRING | POPT_ARG_NONE, &backup_name, 0, _("Backup system files before activating profile"), _("NAME") },
        POPT_TABLEEND
    };

    ret = cli_tool_popt_ex(cmdline, options, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "FEATURE", _("Feature to disable."),
                           &feature, false, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        goto done;
    }

    ret = perform_backup(false, backup, backup_name);
    if (ret != EOK) {
        goto done;
    }

    ret = authselect_feature_disable(feature);
    if (ret != EOK) {
        CLI_ERROR("Unable to disable feature [%d]: %s\n", ret, strerror(ret));
        goto done;
    }

done:
    free(feature);
    return ret;
}

static errno_t is_enabled(struct cli_cmdline *cmdline)
{
    char *feature;
    errno_t ret;

    ret = cli_tool_popt_ex(cmdline, NULL, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "FEATURE", _("Feature to check."),
                           &feature, false, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        goto done;
    }

    ret = authselect_feature_enabled(feature);
    if (ret != EOK && ret != ENOENT) {
        CLI_ERROR("Unable to check feature [%d]: %s\n", ret, strerror(ret));
        goto done;
    }

done:
    free(feature);
    return ret;
}

static errno_t create(struct cli_cmdline *cmdline)
{
    char *name;
    const char *base_id = NULL;
    enum authselect_profile_type type = AUTHSELECT_PROFILE_CUSTOM;
    enum authselect_profile_type base_type = AUTHSELECT_PROFILE_ANY;
    int symlink_flags = AUTHSELECT_SYMLINK_NONE;
    const char **symlinks = NULL;
    char *path = NULL;
    errno_t ret;

    struct poptOption options[] = {
        {"vendor", 'v', POPT_ARG_VAL, &type, AUTHSELECT_PROFILE_VENDOR, _("Create new profile as a vendor profile instead of a custom profile"), NULL },
        {"base-on", 'b', POPT_ARG_STRING, &base_id, 0, _("ID of a profile that should be used as a base for the new profile"), NULL },
        {"base-on-default", '\0', POPT_ARG_VAL, &base_type, AUTHSELECT_PROFILE_DEFAULT, _("Base new profile on a default profile even if vendor profile with the same name exists"), NULL },
        {"symlink-meta", '\0', POPT_ARG_VAL | POPT_ARGFLAG_OR, &symlink_flags, AUTHSELECT_SYMLINK_META, _("Symlink meta files from the base profile instead of copying them"), NULL },
        {"symlink-nsswitch", '\0', POPT_ARG_VAL | POPT_ARGFLAG_OR, &symlink_flags, AUTHSELECT_SYMLINK_NSSWITCH, _("Symlink nsswitch files from the base profile instead of copying them"), NULL },
        {"symlink-pam", '\0', POPT_ARG_VAL | POPT_ARGFLAG_OR, &symlink_flags, AUTHSELECT_SYMLINK_PAM, _("Symlink pam files from the base profile instead of copying them"), NULL },
        {"symlink-dconf", '\0', POPT_ARG_VAL | POPT_ARGFLAG_OR, &symlink_flags, AUTHSELECT_SYMLINK_DCONF, _("Symlink dconf files from the base profile instead of copying them"), NULL },
        {"symlink", 's', POPT_ARG_ARGV, &symlinks, 0, _("Symlink specific file (can be set multiple times)"), NULL },
        POPT_TABLEEND
    };

    ret = cli_tool_popt_ex(cmdline, options, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "NAME", _("New profile name."),
                           &name, false, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        goto done;
    }

    ret = authselect_profile_create(name, type, base_id, base_type,
                                    symlink_flags, symlinks, &path);
    if (ret != EOK) {
        CLI_ERROR("Unable to create new profile [%d]: %s\n", ret, strerror(ret));
        goto done;
    }

    CLI_PRINT("New profile was created at %s\n", path);

done:
    free(path);
    free(name);
    return ret;
}

static errno_t backup_list(struct cli_cmdline *cmdline)
{
    int raw_output = 0;
    char fmttime[255];
    struct stat st;
    struct tm tm;
    char **names;
    char *path;
    int max = 0;
    int len;
    errno_t ret;
    int i;

    struct poptOption options[] = {
        {"raw", 'r', POPT_ARG_VAL, &raw_output, 1,
         _("Print backup names without any formatting and additional information"), NULL },
        POPT_TABLEEND
    };

    ret = cli_tool_popt(cmdline, options, CLI_TOOL_OPT_OPTIONAL, NULL, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        return ret;
    }

    names = authselect_backup_list();
    if (names == NULL) {
        ERROR("Unable to list available backups!");
        return ENOMEM;
    }

    if (raw_output) {
        for (i = 0; names[i] != NULL; i++) {
            printf("%s\n", names[i]);
        }
    } else {
        for (i = 0; names[i] != NULL; i++) {
            len = strlen(names[i]);
            if (max < len) {
                max = len;
            }
        }

        for (i = 0; names[i] != NULL; i++) {
            path = format("%s/%s", AUTHSELECT_BACKUP_DIR, names[i]);
            if (path == NULL) {
                ERROR("Out of memory!");
                ret = ENOMEM;
                goto done;
            }

            ret = stat(path, &st);
            if (ret < 0) {
                ret = errno;
                ERROR("Unable to stat [%s] [%d]: %s", path, ret, strerror(ret));
                free(path);
                goto done;
            }
            free(path);

            localtime_r(&st.st_ctim.tv_sec, &tm);
            memset(fmttime, '\0', sizeof(fmttime));
            strftime(fmttime, 255, "%c", &tm);

            printf(_("%-*s (created at %s)\n"), max, names[i], fmttime);
        }
    }

    ret = EOK;

done:
    authselect_array_free(names);

    return ret;
}

static errno_t backup_remove(struct cli_cmdline *cmdline)
{
    char *name;
    errno_t ret;

    ret = cli_tool_popt_ex(cmdline, NULL, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "BACKUP",
                           _("Name of the backup to remove."),
                           &name, false, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        goto done;
    }

    ret = authselect_backup_remove(name);
    if (ret != EOK) {
        CLI_ERROR("Unable to remove backup [%s] [%d]: %s\n",
                  name, ret, strerror(ret));
        goto done;
    }

done:
    free(name);
    return ret;
}

static errno_t backup_restore(struct cli_cmdline *cmdline)
{
    char *name;
    errno_t ret;

    ret = cli_tool_popt_ex(cmdline, NULL, CLI_TOOL_OPT_OPTIONAL,
                           NULL, NULL, "BACKUP",
                           _("Name of the backup to restore from."),
                           &name, false, NULL);
    if (ret != EOK) {
        ERROR("Unable to parse command arguments");
        goto done;
    }

    ret = authselect_backup_restore(name);
    if (ret != EOK) {
        CLI_ERROR("Unable to restore backup [%s] [%d]: %s\n",
                  name, ret, strerror(ret));
        goto done;
    }

done:
    free(name);
    return ret;
}

static errno_t uninstall(struct cli_cmdline *cmdline)
{
    errno_t ret;

    ret = authselect_uninstall();
    if (ret != EOK) {
        CLI_ERROR("Unable to uninstall authselect configuration [%d]: %s\n",
                  ret, strerror(ret));
        return ret;
    }

    return EOK;
}

static errno_t version(struct cli_cmdline *cmdline)
{
    puts(PACKAGE_VERSION);

    return EOK;
}

static errno_t
setup_gettext()
{
    char *c;

    /* Setup gettext even if we were unable to setup locale. */
    setlocale(LC_ALL, "");

    errno = 0;
    c = bindtextdomain(PACKAGE, LOCALEDIR);
    if (c == NULL) {
        return errno;
    }

    errno = 0;
    c = textdomain(PACKAGE);
    if (c == NULL) {
        return errno;
    }

    return EOK;
}

int main(int argc, const char **argv)
{
    errno_t ret;

    ret = setup_gettext();
    if (ret != EOK) {
        /* We can't use gettext here since it would crash. */
        fprintf(stderr, "Unable to setup gettext!\n");
        return 1;
    }

    struct cli_route_cmd commands[] = {
        CLI_TOOL_COMMAND("select", "Select profile", CLI_CMD_REQUIRE_ROOT, activate),
        CLI_TOOL_COMMAND("apply-changes", "Regenerate configuration for currently selected command", CLI_CMD_REQUIRE_ROOT, apply_changes),
        CLI_TOOL_COMMAND("list", "List available profiles", CLI_CMD_NONE, list),
        CLI_TOOL_COMMAND("list-features", "List available profile features", CLI_CMD_NONE, list_features),
        CLI_TOOL_COMMAND("show", "Show profile information", CLI_CMD_NONE, show),
        CLI_TOOL_COMMAND("requirements", "Print profile requirements", CLI_CMD_NONE, requirements),
        CLI_TOOL_COMMAND("current", "Get identifier of currently selected profile", CLI_CMD_NONE, current),
        CLI_TOOL_COMMAND("check", "Check if the current configuration is valid", CLI_CMD_NONE, check),
        CLI_TOOL_COMMAND("test", "Print changes that would be otherwise written", CLI_CMD_NONE, test),
        CLI_TOOL_COMMAND("enable-feature", "Enable feature in currently selected profile", CLI_CMD_REQUIRE_ROOT, enable),
        CLI_TOOL_COMMAND("disable-feature", "Disable feature in currently selected profile", CLI_CMD_REQUIRE_ROOT, disable),
        CLI_TOOL_COMMAND("is-feature-enabled", "Check if feature is enabled in currently selected profile", CLI_CMD_NONE, is_enabled),
        CLI_TOOL_COMMAND("create-profile", "Create new authselect profile", CLI_CMD_REQUIRE_ROOT, create),
        CLI_TOOL_DELIMITER("Backup commands:"),
        CLI_TOOL_COMMAND("backup-list", "List available backups", CLI_CMD_NONE, backup_list),
        CLI_TOOL_COMMAND("backup-remove", "Remove backup", CLI_CMD_REQUIRE_ROOT, backup_remove),
        CLI_TOOL_COMMAND("backup-restore", "Restore from backup", CLI_CMD_REQUIRE_ROOT, backup_restore),
        CLI_TOOL_DELIMITER("Other:"),
        CLI_TOOL_COMMAND("opt-out", "Opt-out from authselect managed configuration", CLI_CMD_REQUIRE_ROOT, uninstall),
        /* Hidden commands */
        CLI_TOOL_COMMAND("version", "Print authselect version", CLI_CMD_HIDDEN, version),
        CLI_TOOL_LAST
    };

    return cli_tool_main(argc, argv, commands, NULL);
}
