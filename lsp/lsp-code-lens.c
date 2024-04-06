/*
 * Copyright 2023 Jiri Techet <techet@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "lsp/lsp-code-lens.h"
#include "lsp/lsp-utils.h"
#include "lsp/lsp-rpc.h"
#include "lsp/lsp-sync.h"
#include "lsp/lsp-command.h"
#include "lsp/lsp-utils.h"

#include <jsonrpc-glib.h>


extern GeanyPlugin *geany_plugin;
extern GeanyData *geany_data;

static GPtrArray *commands;


static void set_color(GeanyDocument *doc)
{
	LspServerConfig *cfg = lsp_server_get_config(doc);
	GdkColor bg_color, fg_color, color;
	ScintillaObject *sci;
	gint style_offset;
	gint i = 0;
	gchar **comps;

	if (!doc || !cfg)
		return;

	sci = doc->editor->sci;

	style_offset = SSM(sci, SCI_EOLANNOTATIONGETSTYLEOFFSET, 0, 0);

	gdk_color_parse("yellow", &bg_color);
	gdk_color_parse("black", &fg_color);

	comps = g_strsplit(cfg->code_lens_style, ";", -1);

	for (i = 0; comps && comps[i]; i++)
	{
		switch (i)
		{
			case 0:
				if (!gdk_color_parse(comps[i], &color))
					color = fg_color;
				SSM(sci, SCI_STYLESETFORE, style_offset,
					((color.red * 255) / 65535) |
					(((color.green * 255) / 65535) << 8) |
					(((color.blue * 255) / 65535) << 16));
				break;
			case 1:
			{
				if (!gdk_color_parse(comps[i], &color))
					color = bg_color;
				SSM(sci, SCI_STYLESETBACK, style_offset,
					((color.red * 255) / 65535) |
					(((color.green * 255) / 65535) << 8) |
					(((color.blue * 255) / 65535) << 16));
				break;
			}
		}
	}

	g_strfreev(comps);
}


void lsp_code_lens_style_init(GeanyDocument *doc)
{
	LspServerConfig *cfg = lsp_server_get_config(doc);
	ScintillaObject *sci;

	if (!doc || !cfg)
		return;

	sci = doc->editor->sci;

	if (SSM(sci, SCI_EOLANNOTATIONGETSTYLEOFFSET, 0, 0) == 0)
	{
		gint style_offset = SSM(sci, SCI_ALLOCATEEXTENDEDSTYLES, 1, 0);

		SSM(sci, SCI_EOLANNOTATIONSETSTYLEOFFSET, style_offset, 0);
		set_color(doc);

		lsp_code_lens_send_request(doc);
	}

	if (!commands)
		commands = g_ptr_array_new_full(0, (GDestroyNotify)lsp_command_free);
}


static void add_annotation(ScintillaObject *sci, gint line, const gchar *text)
{
	SSM(sci, SCI_EOLANNOTATIONSETTEXT, line, (sptr_t)text);
	SSM(sci, SCI_EOLANNOTATIONSETSTYLE, 0, 0);
	SSM(sci, SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_ANGLE_FLAT, 0);
}


static void code_lens_cb(GVariant *return_value, GError *error, gpointer user_data)
{
	GeanyDocument *doc = user_data;
	gboolean doc_exists = FALSE;
	LspServer *srv;
	gint i;

	foreach_document(i)
	{
		if (doc == documents[i])
		{
			doc_exists = TRUE;
			break;
		}
	}

	srv = doc_exists ? lsp_server_get(doc) : NULL;

	if (!error && srv)
	{
		GVariant *code_action = NULL;
		gint last_line = 0;
		GVariantIter iter;
		GString *str;

		//printf("%s\n\n\n", lsp_utils_json_pretty_print(return_value));

		SSM(doc->editor->sci, SCI_EOLANNOTATIONCLEARALL, 0, 0);

		str = g_string_new(NULL);

		g_variant_iter_init(&iter, return_value);

		while (g_variant_iter_loop(&iter, "v", &code_action))
		{
			const gchar *title = NULL;
			const gchar *command = NULL;
			GVariant *arguments = NULL;
			GVariant *loc_variant = NULL;
			LspCommand *cmd;
			gint line_num = 0;

			JSONRPC_MESSAGE_PARSE(code_action,
				"range", JSONRPC_MESSAGE_GET_VARIANT(&loc_variant)
			);

			if (loc_variant)
			{
				LspRange range = lsp_utils_parse_range(loc_variant);
				line_num = range.start.line;
				g_variant_unref(loc_variant);
			}

			if (!JSONRPC_MESSAGE_PARSE(code_action,
				"command", "{",
					"title", JSONRPC_MESSAGE_GET_STRING(&title),
					"command", JSONRPC_MESSAGE_GET_STRING(&command),
				"}"))
			{
				continue;
			}

			JSONRPC_MESSAGE_PARSE (code_action,
				"command", "{",
					"arguments", JSONRPC_MESSAGE_GET_VARIANT(&arguments),
				"}"
			);

			cmd = g_new0(LspCommand, 1);
			cmd->line = line_num;
			cmd->title = g_strdup(title);
			cmd->command = g_strdup(command);
			cmd->arguments = arguments;

			if (line_num != last_line && str->len > 0)
			{
				add_annotation(doc->editor->sci, last_line, str->str);
				g_string_set_size(str, 0);
			}
			if (str->len == 0)
				g_string_append(str, _("LSP Commands: "));
			else
				g_string_append(str, " | ");
			g_string_append(str, cmd->title);
			last_line = line_num;

			g_ptr_array_add(commands, cmd);
		}

		if (str->len > 0)
			add_annotation(doc->editor->sci, last_line, str->str);

		g_string_free(str, TRUE);

		//printf("%s\n\n\n", lsp_utils_json_pretty_print(return_value));
	}
}


void lsp_code_lens_append_commands(GPtrArray *cmds, guint line)
{
	LspCommand *cmd;
	guint i;

	foreach_ptr_array(cmd, i, commands)
	{
		if (cmd->line == line)
			g_ptr_array_add(cmds, cmd);
	}
}


static gboolean retry_cb(gpointer user_data)
{
	GeanyDocument *doc = user_data;

	//printf("retrying code lens\n");
	if (doc == document_get_current())
	{
		LspServer *srv = lsp_server_get_if_running(doc);
		if (!lsp_server_is_usable(doc))
			;  // server died or misconfigured
		else if (!srv)
			return TRUE;  // retry
		else
		{
			// should be successful now
			lsp_code_lens_send_request(doc);
			return FALSE;
		}
	}

	// server shut down or document not current any more
	return FALSE;
}


void lsp_code_lens_send_request(GeanyDocument *doc)
{
	LspServer *server = lsp_server_get_if_running(doc);
	gchar *doc_uri;
	GVariant *node;

	if (!server)
	{
		// happens when Geany and LSP server started - we cannot send the request yet
		plugin_timeout_add(geany_plugin, 300, retry_cb, doc);
		return;
	}

	if (!server->config.code_lens_enable)
		return;

	g_ptr_array_set_size(commands, 0);

	/* set annotation colors every time - Geany doesn't provide any notification
	 * when color theme changes which also resets colors to some defaults. Even
	 * though we set colors here, it isn't a perfect solution as it needs a modification
	 * of the document for the update and in the meantime the color is wrong. */
	set_color(doc);

	doc_uri = lsp_utils_get_doc_uri(doc);

	/* Geany requests symbols before firing "document-activate" signal so we may
	 * need to request document opening here */
	if (!lsp_sync_is_document_open(doc))
		lsp_sync_text_document_did_open(server, doc);

	node = JSONRPC_MESSAGE_NEW(
		"textDocument", "{",
			"uri", JSONRPC_MESSAGE_PUT_STRING(doc_uri),
		"}"
	);
	lsp_rpc_call(server, "textDocument/codeLens", node,
		code_lens_cb, doc);

	//printf("%s\n\n\n", lsp_utils_json_pretty_print(node));

	g_free(doc_uri);
	g_variant_unref(node);
}
