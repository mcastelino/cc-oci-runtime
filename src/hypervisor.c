/*
 * This file is part of cc-oci-runtime.
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <uuid/uuid.h>

#include "oci.h"
#include "util.h"
#include "hypervisor.h"
#include "common.h"

/** Length of an ASCII-formatted UUID */
#define UUID_MAX 37

/* Values passed in from automake.
 *
 * XXX: They are assigned to variables to allow the tests
 * to modify the values.
 */
private gchar *sysconfdir = SYSCONFDIR;
private gchar *defaultsdir = DEFAULTSDIR;

/*!
 * Generate the expanded kernel network IP configuration
 * hypervisor arguments to pass to the kernel.
 * Currently used to pass in the IP configuration of the
 * first interface.
 *
 * \param config \ref cc_oci_config.
 *
 * \return \c expanded kernel network IP config command line
 */
static gchar *
cc_oci_expand_net_kernel_cmdline(struct cc_oci_config *config) {
	/* www.kernel.org/doc/Documentation/filesystems/nfs/nfsroot.txt
        * ip=<client-ip>:<server-ip>:<gw-ip>:<netmask>:<hostname>:
         * <device>:<autoconf>:<dns0-ip>:<dns1-ip>
	 */

	/* FIXME. Sending multiple ip= does not seem to work
	 * Hence support one or the other for now
	 * Explore dracut or systemd based network init
	 */

	struct cc_oci_net_if_cfg *if_cfg = NULL;

	if (config->net.interfaces == NULL) {
		return ( g_strdup_printf("ip=::::%s::off::",
			config->net.hostname));
	}

	if_cfg = (struct cc_oci_net_if_cfg *) 
		g_slist_nth_data(config->net.interfaces, 0);

	if ( if_cfg->ip_address != NULL ) {
		return ( g_strdup_printf("ip=%s::%s:%s:%s:%s:off::",
			if_cfg->ip_address,
			config->net.gateway,
			if_cfg->subnet_mask,
			config->net.hostname,
			if_cfg->ifname) );
	}

	if ( if_cfg->ipv6_address != NULL ) {
		return ( g_strdup_printf("ip=[%s]::::%s:%s:off::",
			if_cfg->ipv6_address,
			config->net.hostname,
			if_cfg->ifname) );
	}

	return g_strdup("");
}

#define QEMU_FMT_NETDEV "tap,ifname=%s,script=no,downscript=no,id=%s"

/*!
 * Generate the expanded netdev hypervisor arguments to use.
 * for a particular interface
 *
 * \param config \ref cc_oci_net_ifconfig.
 *
 * \return \c expanded 
 */
static gchar *
cc_oci_expand_netdev_cmdline(struct cc_oci_net_if_cfg *config) {

	return g_strdup_printf(QEMU_FMT_NETDEV,
		config->tap_device,
		config->tap_device);
}

#define QEMU_FMT_DEVICE "driver=virtio-net,netdev=%s"
#define QEMU_FMT_DEVICE_MAC QEMU_FMT_DEVICE ",mac=%s"

/*!
 * Generate the expanded device hypervisor arguments to use.
 * for a particular interface
 *
 * \param config \ref cc_oci_net_ifconfig.
 *
 * \return \c expanded device command line
 */
static gchar *
cc_oci_expand_net_device_cmdline(struct cc_oci_net_if_cfg *config) {

	if ( config->mac_address == NULL ) {
		return g_strdup_printf(QEMU_FMT_DEVICE,
			config->tap_device);
	} else {
		return g_strdup_printf(QEMU_FMT_DEVICE_MAC,
			config->tap_device,
			config->mac_address);
	}
}

/*!
 * Return all special tokens related to hyperviso networking
 * parameters
 *
 * \param config \ref cc_oci_config.
 * \param[in, out] netdev_option
 * \param[in, out] netdev_params
 * \param[in, out] net_device_option
 * \param[in, out] net_device_params
 * \param[in, out] netdev2_option
 * \param[in, out] netdev2_params
 * \param[in, out] net_device2_option
 * \param[in, out] net_device2_params
 *
 * \warning this is not very efficient.
 *
 * \return \c true on success, else \c false.
 */

static gboolean
cc_oci_expand_network_cmdline(struct cc_oci_config *config,
			      gchar **netdev_option,
			      gchar **netdev_params,
			      gchar **net_device_option,
			      gchar **net_device_params,
			      gchar **netdev2_option,
			      gchar **netdev2_params,
			      gchar **net_device2_option,
			      gchar **net_device2_params) {

	struct cc_oci_net_if_cfg *if_cfg = NULL;
	guint num_interfaces = 0;
	guint index = 0;

	*netdev_option = NULL;
	*netdev_params = NULL;
	*net_device_option = NULL;
	*net_device_params = NULL;

	if (config->net.interfaces == NULL) {
		/* Support --net=none */
		/* Hacky, no clean way to add/remove args today
		 * For multiple network we need to have a way to append
		 * args to the hypervisor command line vs substitution
		 */
		*netdev_option = g_strdup("-net");
		*netdev_params = g_strdup("none");
		*net_device_option = g_strdup("-net");
		*net_device_params = g_strdup("none");
		return true;
	}


	num_interfaces = g_slist_length(config->net.interfaces);
	g_debug("number of network interfaces %d", num_interfaces);

	for (index = 0; index < num_interfaces; index++) {
		guint i = 0;
		struct cc_oci_net_if_cfg *prev_cfg = NULL;

		if_cfg = (struct cc_oci_net_if_cfg *)
			g_slist_nth_data(config->net.interfaces, index);

		g_debug("processing [%d] [%s]", index, if_cfg->ifname);

		/* skip duplicates */
		for (i=0; i < index; i++) {
			prev_cfg = g_slist_nth_data(config->net.interfaces, i);

			if (!g_strcmp0(if_cfg->ifname, prev_cfg->ifname)) {
				g_debug("skipping duplicate [%d] [%s]",
					i, if_cfg->ifname);
				continue;
			}
		}

		if (*netdev_option == NULL){
			g_debug("first interface [%d] [%s]", index, if_cfg->ifname);
			*netdev_option = g_strdup("-netdev");
			*net_device_option = g_strdup("-device");
			*netdev_params = cc_oci_expand_netdev_cmdline(if_cfg);
			*net_device_params = cc_oci_expand_net_device_cmdline(if_cfg);
			continue;
		}

		if (*netdev2_option == NULL) {
			g_debug("second interface [%d] [%s]", index, if_cfg->ifname);
			*netdev2_option = g_strdup("-netdev");
			*net_device2_option = g_strdup("-device");
			*netdev2_params = cc_oci_expand_netdev_cmdline(if_cfg);
			*net_device2_params = cc_oci_expand_net_device_cmdline(if_cfg);
			/* we support only two for now */
			break;
		}

	}

	if (*netdev2_option == NULL) {
		*netdev2_option = g_strdup("-net");
		*netdev2_params = g_strdup("none");
		*net_device2_option = g_strdup("-net");
		*net_device2_params = g_strdup("none");
	}

	return true;
}


/*!
 * Replace any special tokens found in \p args with their expanded
 * values.
 *
 * \param config \ref cc_oci_config.
 * \param[in, out] args Command-line to expand.
 *
 * \warning this is not very efficient.
 *
 * \return \c true on success, else \c false.
 */
gboolean
cc_oci_expand_cmdline (struct cc_oci_config *config,
		gchar **args)
{
	struct stat       st;
	gchar           **arg;
	gchar            *bytes = NULL;
	gchar            *console_device = NULL;
	g_autofree gchar *procsock_device = NULL;

	gboolean          ret = false;
	gint              count;
	uuid_t            uuid;
	/* uuid pattern */
	const char        uuid_pattern[UUID_MAX] = "00000000-0000-0000-0000-000000000000";
	char              uuid_str[UUID_MAX] = { 0 };
	gint              uuid_index = 0;

	gchar            *kernel_net_params = NULL;
	gchar            *net_device_params = NULL;
	gchar            *netdev_params = NULL;
	gchar            *net_device_option = NULL;
	gchar            *netdev_option = NULL;
	gchar            *net_device2_params = NULL;
	gchar            *netdev2_params = NULL;
	gchar            *net_device2_option = NULL;
	gchar            *netdev2_option = NULL;

	if (! (config && args)) {
		return false;
	}

	if (! config->vm) {
		g_critical ("No vm configuration");
		goto out;
	}

	if (! config->bundle_path) {
		g_critical ("No bundle path");
		goto out;
	}

	/* We're about to launch the hypervisor so validate paths.*/

	if ((!config->vm->image_path[0])
		|| stat (config->vm->image_path, &st) < 0) {
		g_critical ("image file: %s does not exist",
			    config->vm->image_path);
		return false;
	}

	if (!(config->vm->kernel_path[0]
		&& g_file_test (config->vm->kernel_path, G_FILE_TEST_EXISTS))) {
		g_critical ("kernel image: %s does not exist",
			    config->vm->kernel_path);
		return false;
	}

	if (!(config->oci.root.path[0]
		&& g_file_test (config->oci.root.path, G_FILE_TEST_EXISTS|G_FILE_TEST_IS_DIR))) {
		g_critical ("workload directory: %s does not exist",
			    config->oci.root.path);
		return false;
	}

	uuid_generate_random(uuid);
	for(size_t i=0; i<sizeof(uuid_t) && uuid_index < sizeof(uuid_pattern); ++i) {
		/* hex to char */
		uuid_index += g_snprintf(uuid_str+uuid_index,
		                  sizeof(uuid_pattern)-(gulong)uuid_index,
		                  "%02x", uuid[i]);

		/* copy separator '-' */
		if (uuid_pattern[uuid_index] == '-') {
			uuid_index += g_snprintf(uuid_str+uuid_index,
			                  sizeof(uuid_pattern)-(gulong)uuid_index, "-");
		}
	}

	bytes = g_strdup_printf ("%lu", (unsigned long int)st.st_size);

	/* XXX: Note that "signal=off" ensures that the key sequence
	 * CONTROL+c will not cause the VM to exit.
	 */
	if (! config->console || ! g_utf8_strlen(config->console, LINE_MAX)) {

		config->use_socket_console = true;

		/* Temporary fix for non-console output, since -chardev stdio is not working as expected
		 * 
		 * Check if called from docker. Use -chardev pipe as virtualconsole.
		 * Create symlinks to docker named pipes in the format qemu expects.
		 *
		 * Eventually move to using "stdio,id=charconsole0,signal=off"
		 */
		if (! isatty (STDIN_FILENO)) {

			config->console = g_build_path ("/",
					config->bundle_path,
					"cc-std", NULL);

			g_debug ("no console device provided , so using pipe: %s", config->console);

			g_autofree gchar *init_stdout = g_build_path ("/",
							config->bundle_path,
							"init-stdout", NULL);

			g_autofree gchar *cc_stdout = g_build_path ("/",
							config->bundle_path,
							"cc-std.out", NULL);

			g_autofree gchar *init_stdin = g_build_path ("/",
							config->bundle_path,
							"init-stdin", NULL);
			g_autofree gchar *cc_stdin = g_build_path ("/",
							config->bundle_path,
							"cc-std.in", NULL);

			if ( symlink (init_stdout, cc_stdout) == -1) {
				g_critical("Failed to create symlink for output pipe: %s",
					   strerror (errno));
				goto out;
			}

			if ( symlink (init_stdin, cc_stdin) == -1) {
				g_critical("Failed to create symlink for input pipe: %s",
					   strerror (errno));
				goto out;
			}

			console_device = g_strdup_printf ("pipe,id=charconsole0,path=%s", config->console);

		} else {

			/* In case the runtime is called standalone without console */

			/* No console specified, so make the hypervisor create
			 * a Unix domain socket.
			 */	
			config->console = g_build_path ("/",
					config->state.runtime_path,
					CC_OCI_CONSOLE_SOCKET, NULL);

			/* Note that path is not quoted - attempting to do so
			 * results in qemu failing with the error:
			 *
			 *   Failed to bind socket to "/a/dir/console.sock": No such file or directory
			 */

			g_debug ("no console device provided, so using socket: %s", config->console);

			console_device = g_strdup_printf ("socket,path=%s,server,nowait,id=charconsole0,signal=off",
				config->console);
		}
	} else {
		console_device = g_strdup_printf ("serial,id=charconsole0,path=%s", config->console);
	}

	procsock_device = g_strdup_printf ("socket,id=procsock,path=%s,server,nowait", config->state.procsock_path);

	kernel_net_params = cc_oci_expand_net_kernel_cmdline(config);

	cc_oci_expand_network_cmdline(config,
				&netdev_option,
				&netdev_params,
				&net_device_option,
				&net_device_params,
				&netdev2_option,
				&netdev2_params,
				&net_device2_option,
				&net_device2_params);

	/* Note: @NETDEV@: For multiple network we need to have a way to append
	 * args to the hypervisor command line vs substitution
	 */
	struct special_tag {
		const gchar* name;
		const gchar* value;
	} special_tags[] = {
		{ "@WORKLOAD_DIR@"      , config->oci.root.path      },
		{ "@KERNEL@"            , config->vm->kernel_path    },
		{ "@KERNEL_PARAMS@"     , config->vm->kernel_params  },
		/*
		{ "@KERNEL_NET_PARAMS@" , kernel_net_params          },
		*/
		{ "@IMAGE@"             , config->vm->image_path     },
		{ "@SIZE@"              , bytes                      },
		{ "@COMMS_SOCKET@"      , config->state.comms_path   },
		{ "@PROCESS_SOCKET@"    , procsock_device            },
		{ "@CONSOLE_DEVICE@"    , console_device             },
		{ "@NAME@"              , g_strrstr(uuid_str, "-")+1 },
		{ "@UUID@"              , uuid_str                   },
		{ "@NETDEV@"            , netdev_option              },
		{ "@NETDEV_PARAMS@"     , netdev_params              },
		{ "@NETDEVICE@"         , net_device_option          },
		{ "@NETDEVICE_PARAMS@"  , net_device_params          },
		{ "@NETDEV2@"           , netdev2_option             },
		{ "@NETDEV2_PARAMS@"    , netdev2_params             },
		{ "@NETDEVICE2@"        , net_device2_option         },
		{ "@NETDEVICE2_PARAMS@" , net_device2_params         },
		{ NULL }
	};

	for (arg = args, count = 0; arg && *arg; arg++, count++) {
		if (! count) {
			/* command must be the first entry */
			if (! g_path_is_absolute (*arg)) {
				gchar *cmd = g_find_program_in_path (*arg);

				if (cmd) {
					g_free (*arg);
					*arg = cmd;
				}
			}
		}

		/* when first character is '#' line is a comment and must be ignored */
		if (**arg == '#') {
			g_strlcpy(*arg, "\0", LINE_MAX);
			continue;
		}

		/* looking for '#' */
		gchar* ptr = g_strstr_len(*arg, LINE_MAX, "#");
		while (ptr) {
			/* if '[:space:]#' then replace '#' with '\0' (EOL) */
			if (g_ascii_isspace(*(ptr-1))) {
				g_strlcpy(ptr, "\0", LINE_MAX);
				break;
			}
			ptr = g_strstr_len(ptr+1, LINE_MAX, "#");
		}

		for (struct special_tag* tag=special_tags; tag && tag->name; tag++) {
			if (! cc_oci_replace_string(arg, tag->name, tag->value)) {
				goto out;
			}
		}
	}

	ret = true;

out:
	g_free_if_set (bytes);
	g_free_if_set (console_device);
	g_free_if_set (kernel_net_params);
	g_free_if_set (net_device_params);
	g_free_if_set (netdev_params);
	g_free_if_set (net_device_option);
	g_free_if_set (netdev_option);

	return ret;
}

/*!
 * Determine the full path to the \ref CC_OCI_HYPERVISOR_CMDLINE_FILE
 * file.
 * Priority order to get file path : bundle dir, sysconfdir , defaultsdir
 *
 * \param config \ref cc_oci_config.
 *
 * \return Newly-allocated string on success, else \c NULL.
 */
private gchar *
cc_oci_vm_args_file_path (const struct cc_oci_config *config)
{
	gchar *args_file = NULL;

	if (! config) {
		return NULL;
	}

	if (! config->bundle_path) {
		return NULL;
	}

	args_file = cc_oci_get_bundlepath_file (config->bundle_path,
			CC_OCI_HYPERVISOR_CMDLINE_FILE);
	if (! args_file) {
		return NULL;
	}

	if (g_file_test (args_file, G_FILE_TEST_EXISTS)) {
		goto out;
	}

	g_free_if_set (args_file);

	/* Try sysconfdir if bundle file does not exist */
	args_file = g_build_path ("/", sysconfdir,
			CC_OCI_HYPERVISOR_CMDLINE_FILE, NULL);

	if (g_file_test (args_file, G_FILE_TEST_EXISTS)) {
		goto out;
	}

	g_free_if_set (args_file);

	/* Finally, try stateless dir */
	args_file = g_build_path ("/", defaultsdir,
			CC_OCI_HYPERVISOR_CMDLINE_FILE, NULL);

	if (g_file_test (args_file, G_FILE_TEST_EXISTS)) {
		goto out;
	}

	g_free_if_set (args_file);

	/* no file found, so give up */
	args_file = NULL;

out:
	g_debug ("using %s", args_file);
	return args_file;
}

/*!
 * Generate the unexpanded list of hypervisor arguments to use.
 *
 * \param config \ref cc_oci_config.
 * \param[out] args Command-line to expand.
 *
 * \return \c true on success, else \c false.
 */
gboolean
cc_oci_vm_args_get (struct cc_oci_config *config,
		gchar ***args)
{
	gboolean  ret;
	gchar    *args_file = NULL;
	guint     line_count = 0;
	gchar   **arg;
	gchar   **new_args;

	if (! (config && args)) {
		return false;
	}

	args_file = cc_oci_vm_args_file_path (config);
	if (! args_file) {
		g_critical("File %s not found",
				CC_OCI_HYPERVISOR_CMDLINE_FILE);
	}

	ret = cc_oci_file_to_strv (args_file, args);
	if (! ret) {
		goto out;
	}

	ret = cc_oci_expand_cmdline (config, *args);
	if (! ret) {
		goto out;
	}

	/* count non-empty lines */
	for (arg = *args; arg && *arg; arg++) {
		if (**arg != '\0') {
			line_count++;
		}
	}

	new_args = g_malloc0(sizeof(gchar*) * (line_count+1));

	/* copy non-empty lines */
	for (arg = *args, line_count = 0; arg && *arg; arg++) {
		/* *do not* add empty lines */
		if (**arg != '\0') {
			/* container fails if arg contains spaces */
			g_strstrip(*arg);
			new_args[line_count] = *arg;
			line_count++;
		} else {
			/* free empty lines */
			g_free(*arg);
		}
	}

	/* only free pointer to gchar* */
	g_free(*args);

	/* copy new args */
	*args = new_args;

	ret = true;
out:
	g_free_if_set (args_file);
	return ret;
}
