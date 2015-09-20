/*
   american fuzzy lop - GCC instrumentation pass
   ---------------------------------------------

   Written by Austin Seipp <aseipp@pobox.com> with bits from
              Emese Revfy <re.emese@gmail.com>

   GCC integration design is based on the LLVM design, which comes
   from Laszlo Szekeres. Some of the boilerplate code below for
   afl_pass to adapt to different GCC versions was taken from Emese
   Revfy's Size Overflow plugin for GCC, licensed under the GPLv2/v3.

   (NOTE: this plugin code is under GPLv3, in order to comply with the
   GCC runtime library exception, which states that you may distribute
   "Target Code" from the compiler under a license of your choice, as
   long as the "Compilation Process" is "Eligible", and contains no
   GPL-incompatible software in GCC "during the process of
   transforming high level code to target code". In this case, the
   plugin will be used to generate "Target Code" during the
   "Compilation Process", and thus it must be GPLv3 to be "eligible".)

   Copyright (C) 2015 Austin Seipp

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <diagnostic.h>
#include <tree.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <basic-block.h>
#include <tree-pass.h>
#include <version.h>
#include <toplev.h>
#include <intl.h>
#include <stringpool.h>
#include <context.h>

/* -------------------------------------------------------------------------- */
/* -- AFL instrumentation pass ---------------------------------------------- */

static int be_quiet = 0;
static unsigned int inst_ratio = 100;
static int inst_blocks = 0;

/* -------------------------------------------------------------------------- */
/* -- Boilerplate and initialization ---------------------------------------- */

static const pass_data my_pass_data = {
  /* type */ GIMPLE_PASS,
  /* name */ "afl-inst",
  /* optinfo_flags */ 0,
  /* tv_id */ TV_NONE,
  /* pops required */ 0,
  /* pops provided */ 0,
  /* pops destroyed */ 0,

  /* todo_flags_start */ 0,
  // NOTE(aseipp): it's very, very important to include
  // at least 'TODO_update_ssa' here so that GCC will
  // properly update the resulting SSA form e.g. to
  // include new PHI nodes for newly added symbols or
  // names. Do not remove this.
  /* todo_flags_finish */ TODO_update_ssa | TODO_verify_all | TODO_cleanup_cfg,
};

class afl_pass : public gimple_opt_pass
{
public:
  afl_pass(gcc::context * ctx) :
    gimple_opt_pass(my_pass_data, ctx)
  {
  }

  virtual unsigned int execute (function *fun)
  {
    /* Instrument all the things! */
    basic_block bb;

    FOR_EACH_BB_FN(bb, fun) {
      gimple_seq seq = NULL;

      /* Bail on this block if we trip the specified ratio */

      if (R(100) >= inst_ratio) continue;

      /* Make up cur_loc */

      unsigned int rand_loc = R(MAP_SIZE);
      tree cur_loc = build_int_cst(uint64_type_node, rand_loc);

      /* Update bitmap via external call */

      tree fntype = build_function_type_list(
          void_type_node,   /* return */
          uint16_type_node, /* args */
          NULL_TREE);
      tree fndecl = build_fn_decl("__afl_trace", fntype);
      TREE_STATIC(fndecl)     = 1; /* Defined elsewhere */
      TREE_PUBLIC(fndecl)     = 1; /* Public */
      DECL_EXTERNAL(fndecl)   = 1; /* External linkage */
      DECL_ARTIFICIAL(fndecl) = 1; /* Injected by compiler */

      auto g = gimple_build_call(fndecl, 1, cur_loc);
      gimple_seq_add_stmt(&seq, g);

      /* Done - grab the entry to the block and insert sequence */

      auto bentry = gsi_start_bb(bb);
      gsi_insert_seq_before(&bentry, seq, GSI_SAME_STMT);

      inst_blocks++;
    }

    /* Say something nice. */
    if (!be_quiet) {
      if (!inst_blocks) WARNF(G_("No instrumentation targets found."));
      else OKF(G_("Instrumented %u locations (%s mode, ratio %u%%)."),
          inst_blocks,
          getenv("AFL_HARDEN") ? G_("hardened") : G_("non-hardened"),
          inst_ratio);
    }

    return 0;
  }
};

/* -------------------------------------------------------------------------- */
/* -- Initialization -------------------------------------------------------- */

int plugin_is_GPL_compatible = 1;

static struct plugin_info afl_plugin_info = {
  .version = "20170625",
  .help    = "AFL gcc plugin\n",
};

int plugin_init(plugin_name_args *plugin_info,
    plugin_gcc_version *version)
{
  {
    struct timeval tv;
    struct timezone tz;

    /* Setup random() so we get Actually Random(TM) outputs from R() */
    gettimeofday(&tv, &tz);
    u32 rand_seed = tv.tv_sec ^ tv.tv_usec ^ getpid();
    srandom(rand_seed);
  }

  /* Pass information */
  register_pass_info afl_pass_info;
  afl_pass_info.pass                      = new afl_pass(g);
  afl_pass_info.reference_pass_name       = "ssa";
  afl_pass_info.ref_pass_instance_number  = 1;
  afl_pass_info.pos_op                    = PASS_POS_INSERT_AFTER;

  if (!plugin_default_version_check(version, &gcc_version)) {
    FATAL(G_("Incompatible gcc/plugin versions!"));
  }

  /* Show a banner */
  if (isatty(2) && !getenv("AFL_QUIET")) {
    SAYF(G_(cCYA "afl-gcc-pass " cBRI VERSION cRST " by <aseipp@pobox.com>\n"));
  } else be_quiet = 1;

  /* Decide instrumentation ratio */
  char* inst_ratio_str = getenv("AFL_INST_RATIO");

  if (inst_ratio_str) {
    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL(G_("Bad value of AFL_INST_RATIO (must be between 1 and 100)"));
  }

  auto pname = plugin_info->base_name;
  register_callback(pname, PLUGIN_INFO, NULL, &afl_plugin_info);
  register_callback(pname, PLUGIN_PASS_MANAGER_SETUP, NULL, &afl_pass_info);

  return 0;
}
