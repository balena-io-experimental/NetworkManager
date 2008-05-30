/*
 *  Copyright (C) 2004 - 2008 Red Hat, Inc.
 *
 *  Written by Colin Walters <walters@redhat.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <glib.h>

#include <glib/gi18n.h>

#include "nm-named-manager.h"
#include "nm-ip4-config.h"
#include "nm-utils.h"
#include "NetworkManagerSystem.h"

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#ifndef RESOLV_CONF
#define RESOLV_CONF "/etc/resolv.conf"
#endif

G_DEFINE_TYPE(NMNamedManager, nm_named_manager, G_TYPE_OBJECT)

#define NM_NAMED_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), \
                                         NM_TYPE_NAMED_MANAGER, \
                                         NMNamedManagerPrivate))


struct NMNamedManagerPrivate {
	NMIP4Config *   vpn_config;
	NMIP4Config *   device_config;
	GSList *        configs;

	gboolean disposed;
};


NMNamedManager *
nm_named_manager_get (void)
{
	static NMNamedManager * singleton = NULL;

	if (!singleton) {
		singleton = NM_NAMED_MANAGER (g_object_new (NM_TYPE_NAMED_MANAGER, NULL));
	} else {
		g_object_ref (singleton);
	}

	g_assert (singleton);
	return singleton;
}


GQuark
nm_named_manager_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("nm_named_manager_error");

	return quark;
}

static char *
compute_nameservers (NMIP4Config *config)
{
	int i, num;
	GString *str = NULL;

	g_return_val_if_fail (config != NULL, NULL);

	num = nm_ip4_config_get_num_nameservers (config);
	if (num == 0)
		return NULL;

	str = g_string_new ("");
	for (i = 0; i < num; i++) {
		#define ADDR_BUF_LEN 50
		struct in_addr addr;
		char *buf;

		addr.s_addr = nm_ip4_config_get_nameserver (config, i);
		buf = g_malloc0 (ADDR_BUF_LEN);
		if (!buf)
			continue;

		inet_ntop (AF_INET, &addr, buf, ADDR_BUF_LEN);

		if (i == 3) {
			g_string_append (str, "\n# ");
			g_string_append (str, _("NOTE: the glibc resolver does not support more than 3 nameservers."));
			g_string_append (str, "\n# ");
			g_string_append (str, _("The nameservers listed below may not be recognized."));
			g_string_append_c (str, '\n');
		}

		g_string_append (str, "nameserver ");
		g_string_append (str, buf);
		g_string_append_c (str, '\n');
		g_free (buf);
	}

	return g_string_free (str, FALSE);
}

static void
merge_one_ip4_config (NMIP4Config *dst, NMIP4Config *src)
{
	guint32 num, num_domains, i;

	num = nm_ip4_config_get_num_nameservers (src);
	for (i = 0; i < num; i++)
		nm_ip4_config_add_nameserver (dst, nm_ip4_config_get_nameserver (src, i));

	num_domains = nm_ip4_config_get_num_domains (src);
	for (i = 0; i < num_domains; i++)
		nm_ip4_config_add_domain (dst, nm_ip4_config_get_domain (src, i));

	num = nm_ip4_config_get_num_searches (src);
	if (num > 0) {
		for (i = 0; i < num; i++)
			nm_ip4_config_add_search (dst, nm_ip4_config_get_search (src, i));
	} else {
		/* If no search domains were specified, add the 'domain' list to
		 * search domains.
		 */
		for (i = 0; i < num_domains; i++)
			nm_ip4_config_add_search (dst, nm_ip4_config_get_domain (src, i));
	}
}

static gboolean
rewrite_resolv_conf (NMNamedManager *mgr, GError **error)
{
	NMNamedManagerPrivate *priv;
	const char *tmp_resolv_conf = RESOLV_CONF ".tmp";
	char *searches = NULL, *domain = NULL;
	char *nameservers = NULL;
	guint32 num_domains, num_searches, i;
	NMIP4Config *composite;
	GSList *iter;
	FILE *f;
	GString *str;

	g_return_val_if_fail (error != NULL, FALSE);
	g_return_val_if_fail (*error == NULL, FALSE);

	priv = NM_NAMED_MANAGER_GET_PRIVATE (mgr);

	/* If the sysadmin disabled modifying resolv.conf, exit silently */
	if (!nm_system_should_modify_resolv_conf ()) {
		nm_info ("DHCP returned name servers but system has disabled dynamic modification!");
		return TRUE;
	}

	if ((f = fopen (tmp_resolv_conf, "w")) == NULL) {
		g_set_error (error,
			     NM_NAMED_MANAGER_ERROR,
			     NM_NAMED_MANAGER_ERROR_SYSTEM,
			     "Could not open " RESOLV_CONF ": %s\n",
			     g_strerror (errno));
		return FALSE;
	}

	if (fprintf (f, "%s","# generated by NetworkManager, do not edit!\n\n") < 0) {
		g_set_error (error,
			     NM_NAMED_MANAGER_ERROR,
			     NM_NAMED_MANAGER_ERROR_SYSTEM,
			     "Could not write " RESOLV_CONF ": %s\n",
			     g_strerror (errno));
		fclose (f);
		return FALSE;
	}

	/* Construct the composite config from all the currently active IP4Configs */
	composite = nm_ip4_config_new ();

	if (priv->vpn_config)
		merge_one_ip4_config (composite, priv->vpn_config);

	if (priv->device_config)
		merge_one_ip4_config (composite, priv->device_config);

	for (iter = priv->configs; iter; iter = g_slist_next (iter)) {
		NMIP4Config *config = NM_IP4_CONFIG (iter->data);

		if ((config == priv->vpn_config) || (config == priv->device_config))
			continue;

		merge_one_ip4_config (composite, config);
	}

	/* ISC DHCP 3.1 provides support for the domain-search option. This is the
	 * correct way for a DHCP server to provide a domain search list. Wedging
	 * multiple domains into the domain-name option is a horrible hack.
	 *
	 * So, we handle it like this (as proposed by Andrew Pollock at
	 * http://bugs.debian.org/465158):
	 *
	 * - if the domain-search option is present in the data received via DHCP,
	 *   use it in favour of the domain-name option for setting the search
	 *   directive in /etc/resolv.conf
	 *
	 * - if the domain-name option is present in the data received via DHCP, use
	 *   it to set the domain directive in /etc/resolv.conf
	 *   (this is handled in compute_domain() below)
	 *
	 * - if only the domain-name option is present in the data received via DHCP
	 *   (and domain-search is not), for backwards compatibility, set the search
	 *   directive in /etc/resolv.conf to the specified domain names
	 */

	num_domains = nm_ip4_config_get_num_domains (composite);
	num_searches = nm_ip4_config_get_num_searches (composite);

	if ((num_searches == 0)  && (num_domains > 0)) {
		str = g_string_new ("search");
		for (i = 0; i < num_domains; i++) {
			g_string_append_c (str, ' ');
			g_string_append (str, nm_ip4_config_get_domain (composite, i));		
		}

		g_string_append (str, "\n\n");
		searches = g_string_free (str, FALSE);
	} else if (num_searches > 0) {
		str = g_string_new ("search");
		for (i = 0; i < num_searches; i++) {
			g_string_append_c (str, ' ');
			g_string_append (str, nm_ip4_config_get_search (composite, i));		
		}

		g_string_append (str, "\n\n");
		searches = g_string_free (str, FALSE);
	}

	/* Compute resolv.conf domain */
	if (num_domains > 0) {
		str = g_string_new ("domain ");
		g_string_append (str, nm_ip4_config_get_domain (composite, 0));
		g_string_append (str, "\n\n");

		domain = g_string_free (str, FALSE);
	}

	/* Using glibc resolver */
	nameservers = compute_nameservers (composite);
	if (fprintf (f, "%s%s%s\n",
	             domain ? domain : "",
	             searches ? searches : "",
	             nameservers ? nameservers : "") < 0) {
		g_set_error (error,
			     NM_NAMED_MANAGER_ERROR,
			     NM_NAMED_MANAGER_ERROR_SYSTEM,
			     "Could not write to " RESOLV_CONF ": %s\n",
			     g_strerror (errno));
	}
	g_free (nameservers);

	if (fclose (f) < 0) {
		if (*error == NULL) {
			g_set_error (error,
				     NM_NAMED_MANAGER_ERROR,
				     NM_NAMED_MANAGER_ERROR_SYSTEM,
				     "Could not close " RESOLV_CONF ": %s\n",
				     g_strerror (errno));
		}
	}

	g_free (domain);
	g_free (searches);

	if (*error == NULL) {
		if (rename (tmp_resolv_conf, RESOLV_CONF) < 0) {
			g_set_error (error,
				     NM_NAMED_MANAGER_ERROR,
				     NM_NAMED_MANAGER_ERROR_SYSTEM,
				     "Could not replace " RESOLV_CONF ": %s\n",
				     g_strerror (errno));
		} else {
			nm_system_update_dns ();
		}
	}

	return *error ? FALSE : TRUE;
}

gboolean
nm_named_manager_add_ip4_config (NMNamedManager *mgr,
                                 NMIP4Config *config,
                                 NMNamedIPConfigType cfg_type)
{
	NMNamedManagerPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (mgr != NULL, FALSE);
	g_return_val_if_fail (config != NULL, FALSE);

	priv = NM_NAMED_MANAGER_GET_PRIVATE (mgr);

	switch (cfg_type) {
	case NM_NAMED_IP_CONFIG_TYPE_VPN:
		priv->vpn_config = config;
		break;
	case NM_NAMED_IP_CONFIG_TYPE_BEST_DEVICE:
		priv->device_config = config;
		break;
	default:
		break;
	}

	/* Don't allow the same zone added twice */
	if (!g_slist_find (priv->configs, config))
		priv->configs = g_slist_append (priv->configs, g_object_ref (config));

	if (!rewrite_resolv_conf (mgr, &error)) {
		nm_warning ("Could not commit DNS changes.  Error: '%s'", error ? error->message : "(none)");
		g_error_free (error);
	}

	return TRUE;
}

gboolean
nm_named_manager_remove_ip4_config (NMNamedManager *mgr, NMIP4Config *config)
{
	NMNamedManagerPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (mgr != NULL, FALSE);
	g_return_val_if_fail (config != NULL, FALSE);

	priv = NM_NAMED_MANAGER_GET_PRIVATE (mgr);

	/* Can't remove it if it wasn't in the list to begin with */
	if (!g_slist_find (priv->configs, config))
		return FALSE;

	priv->configs = g_slist_remove (priv->configs, config);

	if (config == priv->vpn_config)
		priv->vpn_config = NULL;

	if (config == priv->device_config)
		priv->device_config = NULL;

	g_object_unref (config);	

	if (!rewrite_resolv_conf (mgr, &error)) {
		nm_warning ("Could not commit DNS changes.  Error: '%s'", error ? error->message : "(none)");
		if (error)
			g_error_free (error);
	}

	return TRUE;
}


static void
nm_named_manager_init (NMNamedManager *mgr)
{
}

static void
nm_named_manager_finalize (GObject *object)
{
	NMNamedManagerPrivate *priv = NM_NAMED_MANAGER_GET_PRIVATE (object);

	g_slist_foreach (priv->configs, (GFunc) g_object_unref, NULL);
	g_slist_free (priv->configs);

	G_OBJECT_CLASS (nm_named_manager_parent_class)->finalize (object);
}

static void
nm_named_manager_class_init (NMNamedManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = nm_named_manager_finalize;

	g_type_class_add_private (object_class, sizeof (NMNamedManagerPrivate));
}

