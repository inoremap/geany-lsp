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

#include "lsp-command.h"
#include "lsp-utils.h"
#include "lsp-rpc.h"
#include "lsp-diagnostics.h"

#include <jsonrpc-glib.h>


static GPtrArray *code_actions;


void lsp_command_free(LspCommand *cmd)
{
	g_free(cmd->title);
	g_free(cmd->command);
	if (cmd->arguments)
		g_variant_unref(cmd->arguments);
	if (cmd->edit)
		g_variant_unref(cmd->edit);
	g_free(cmd);
}


void lsp_command_send_code_action_destroy(void)
{
	if (code_actions)
		g_ptr_array_free(code_actions, TRUE);
	code_actions = NULL;
}


void lsp_command_send_code_action_init(void)
{
	lsp_command_send_code_action_destroy();
	code_actions = g_ptr_array_new_full(0, (GDestroyNotify)lsp_command_free);
}


GPtrArray *lsp_command_get_resolved_code_actions(void)
{
	return code_actions;
}


static void command_cb(GVariant *return_value, GError *error, gpointer user_data)
{
	if (!error)
	{
		//printf("%s\n\n\n", lsp_utils_json_pretty_print(return_value));
	}
}


void lsp_command_perform(LspServer *server, LspCommand *cmd)
{
	if (cmd->edit)
		lsp_utils_apply_workspace_edit(cmd->edit);

	if (cmd->command)
	{
		GVariant *node;

		if (cmd->arguments)
		{
			GVariantDict dict;

			g_variant_dict_init(&dict, NULL);
			g_variant_dict_insert_value(&dict, "command", g_variant_new_string(cmd->command));
			g_variant_dict_insert_value(&dict, "arguments", cmd->arguments);
			node = g_variant_take_ref(g_variant_dict_end(&dict));
		}
		else
		{
			node = JSONRPC_MESSAGE_NEW(
				"command", JSONRPC_MESSAGE_PUT_STRING(cmd->command)
			);
		}

		//printf("%s\n\n\n", lsp_utils_json_pretty_print(node));

		lsp_rpc_call(server, "workspace/executeCommand", node,
			command_cb, NULL);

		g_variant_unref(node);
	}
}


static void code_action_cb(GVariant *return_value, GError *error, gpointer user_data)
{
	GCallback callback = user_data;

	if (error)
		return;

	if (g_variant_is_of_type(return_value, G_VARIANT_TYPE_ARRAY))
	{
		GVariant *code_action = NULL;
		GVariantIter iter;

		//printf("%s\n\n\n", lsp_utils_json_pretty_print(return_value));

		g_variant_iter_init(&iter, return_value);

		while (g_variant_iter_loop(&iter, "v", &code_action))
		{
			const gchar *title = NULL;
			const gchar *command = NULL;
			GVariant *edit = NULL;
			LspCommand *cmd;
			gboolean is_command;

			// Can either be Command or CodeAction:
			//   Command {title: string; command: string; arguments?: LSPAny[];}
			//   CodeAction {title: string; edit?: WorkspaceEdit; command?: Command;}

			JSONRPC_MESSAGE_PARSE(code_action,
				"title", JSONRPC_MESSAGE_GET_STRING(&title)
			);

			is_command = JSONRPC_MESSAGE_PARSE(code_action,
				"command", JSONRPC_MESSAGE_GET_STRING(&command)
			);

			if (!is_command)
			{
				JSONRPC_MESSAGE_PARSE(code_action,
					"command", "{",
						"command", JSONRPC_MESSAGE_GET_STRING(&command),
					"}"
				);

				JSONRPC_MESSAGE_PARSE(code_action,
					"edit", JSONRPC_MESSAGE_GET_VARIANT(&edit)
				);
			}

			if (title && (command || edit))
			{
				GVariant *arguments = NULL;

				if (is_command)
				{
					JSONRPC_MESSAGE_PARSE(code_action,
						"arguments", JSONRPC_MESSAGE_GET_VARIANT(&arguments)
					);
				}
				else
				{
					JSONRPC_MESSAGE_PARSE(code_action,
						"command", "{",
							"arguments", JSONRPC_MESSAGE_GET_VARIANT(&arguments),
						"}"
					);
				}

				cmd = g_new0(LspCommand, 1);
				cmd->title = g_strdup(title);
				cmd->command = g_strdup(command);
				cmd->arguments = arguments;
				cmd->edit = edit;

				g_ptr_array_add(code_actions, cmd);
			}
			else
			{
				if (edit)
					g_variant_unref(edit);
			}
		}
	}

	callback();
}


void lsp_command_send_code_action_request(gint pos, GCallback actions_resolved_cb)
{
	GeanyDocument *doc = document_get_current();
	LspServer *srv = lsp_server_get_if_running(doc);
	GVariant *diag_raw = lsp_diagnostics_get_diag_raw(pos);
	GVariant *node, *diagnostics, *diags_dict;
	LspPosition lsp_pos_start, lsp_pos_end;
	gint pos_start, pos_end;
	ScintillaObject *sci;
	GVariantDict dict;
	GPtrArray *arr;
	gchar *doc_uri;

	lsp_command_send_code_action_init();

	if (!srv)
	{
		actions_resolved_cb();
		return;
	}

	sci = doc->editor->sci;

	pos_start = sci_get_selection_start(sci);
	pos_end = sci_get_selection_end(sci);

	lsp_pos_start = lsp_utils_scintilla_pos_to_lsp(sci, pos_start);
	lsp_pos_end = lsp_utils_scintilla_pos_to_lsp(sci, pos_end);

	arr = g_ptr_array_new_full(1, (GDestroyNotify) g_variant_unref);
	if (diag_raw)
		g_ptr_array_add(arr, g_variant_ref(diag_raw));
	diagnostics = g_variant_new_array(G_VARIANT_TYPE_VARDICT,
		(GVariant **)arr->pdata, arr->len);

	g_variant_dict_init(&dict, NULL);
	g_variant_dict_insert_value(&dict, "diagnostics", diagnostics);
	diags_dict = g_variant_take_ref(g_variant_dict_end(&dict));

	doc_uri = lsp_utils_get_doc_uri(doc);

	node = JSONRPC_MESSAGE_NEW (
		"textDocument", "{",
			"uri", JSONRPC_MESSAGE_PUT_STRING(doc_uri),
		"}",
		"range", "{",
			"start", "{",
				"line", JSONRPC_MESSAGE_PUT_INT32(lsp_pos_start.line),
				"character", JSONRPC_MESSAGE_PUT_INT32(lsp_pos_start.character),
			"}",
			"end", "{",
				"line", JSONRPC_MESSAGE_PUT_INT32(lsp_pos_end.line),
				"character", JSONRPC_MESSAGE_PUT_INT32(lsp_pos_end.character),
			"}",
		"}",
		"context", "{",
			JSONRPC_MESSAGE_PUT_VARIANT(diags_dict),
		"}"
	);

	//printf("%s\n\n\n", lsp_utils_json_pretty_print(node));

	lsp_rpc_call(srv, "textDocument/codeAction", node, code_action_cb,
		actions_resolved_cb);

	g_variant_unref(node);
	g_variant_unref(diags_dict);
	g_free(doc_uri);
}
