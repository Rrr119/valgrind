/*--------------------------------------------------------------------*/
/*--- MemAccess: a memory access pattern profiler.      ma_main.c ---*/
/*--------------------------------------------------------------------*/

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_options.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_machine.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_oset.h"
#include "pub_tool_vki.h"

/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

static const HChar* clo_flamegraph_out_file = "memaccess.out.%p";
static UInt         clo_sequential_threshold = 90;

static Bool ma_process_cmd_line_option(const HChar* arg)
{
   if VG_STR_CLO(arg, "--flamegraph-out-file", clo_flamegraph_out_file) {}
   else if VG_BINT_CLO(arg, "--sequential-threshold",
                       clo_sequential_threshold, 0, 100) {}
   else
      return False;

   return True;
}

static void ma_print_usage(void)
{
   VG_(printf)(
"    --flamegraph-out-file=<file>    write folded flame graph data [memaccess.out.%%p]\n"
"    --sequential-threshold=<0..100> minimum dominant-stride percentage [90]\n"
   );
}

static void ma_print_debug_usage(void)
{
   VG_(printf)("    (none)\n");
}

/*------------------------------------------------------------*/
/*--- Access stream statistics                             ---*/
/*------------------------------------------------------------*/

/* A random stream can contain an unbounded number of distinct strides.
   Keep a bounded heavy-hitter table so helper calls never need to
   allocate memory.  Once the table is full, the least frequent entry is
   replaced using the Space-Saving rule. */
#define N_STRIDE_BINS 32

typedef
   enum {
      Pattern_None,
      Pattern_Sequential,
      Pattern_Strided,
      Pattern_Random
   }
   AccessPattern;

typedef
   struct {
      Long  stride;
      ULong count;
   }
   StrideBin;

typedef
   struct {
      Addr          prev_addr;
      Bool          have_prev;
      ULong         accesses;
      ULong         stride_samples;
      ULong         sequential_steps;
      StrideBin     strides[N_STRIDE_BINS];
      UInt          n_strides;
      AccessPattern pattern;
      Long          dominant_stride;
   }
   AccessStream;

typedef
   struct {
      HChar* name;             /* The key must be the first field. */
      AccessStream reads;
      AccessStream writes;
   }
   FunctionStats;

static OSet* functions;

static Word function_cmp(const void* key, const void* elem)
{
   const HChar* key_name  = *(const HChar* const*)key;
   const HChar* elem_name = *(const HChar* const*)elem;
   return (Word)VG_(strcmp)(key_name, elem_name);
}

static void initialize_stream(AccessStream* stream)
{
   VG_(memset)(stream, 0, sizeof(*stream));
   stream->pattern = Pattern_None;
   stream->dominant_stride = 0;
}

static FunctionStats* new_function_stats(const HChar* name)
{
   FunctionStats* stats;
   HChar* copy;

   stats = VG_(OSetGen_AllocNode)(functions, sizeof(*stats));
   VG_(memset)(stats, 0, sizeof(*stats));

   copy = VG_(strdup)("memaccess.fn.name", name);
   stats->name = copy;
   initialize_stream(&stats->reads);
   initialize_stream(&stats->writes);

   VG_(OSetGen_Insert)(functions, stats);
   return stats;
}

static FunctionStats* get_function_stats(Addr ip)
{
   const HChar* name = "???";
   FunctionStats* stats;
   DiEpoch ep = VG_(current_DiEpoch)();

   VG_(get_fnname)(ep, ip, &name);
   if (name == NULL || name[0] == '\0') {
      name = "???";
   }

   stats = VG_(OSetGen_Lookup)(functions, &name);
   if (stats == NULL) {
      stats = new_function_stats(name);
   }
   return stats;
}

static FunctionStats* unknown_function_stats(void)
{
   const HChar* unknown_name = "???";
   FunctionStats* stats = VG_(OSetGen_Lookup)(functions, &unknown_name);

   if (stats == NULL) {
      stats = new_function_stats(unknown_name);
   }
   return stats;
}

static UInt least_frequent_stride_index(const AccessStream* stream)
{
   UInt i;
   UInt least = 0;

   tl_assert(stream->n_strides > 0);
   for (i = 1; i < stream->n_strides; i++) {
      if (stream->strides[i].count < stream->strides[least].count) {
         least = i;
      }
   }
   return least;
}

static void add_stride_sample(AccessStream* stream, Long stride)
{
   UInt i;

   for (i = 0; i < stream->n_strides; i++) {
      if (stream->strides[i].stride == stride) {
         stream->strides[i].count++;
         return;
      }
   }

   if (stream->n_strides < N_STRIDE_BINS) {
      stream->strides[stream->n_strides].stride = stride;
      stream->strides[stream->n_strides].count = 1;
      stream->n_strides++;
   } else {
      UInt least = least_frequent_stride_index(stream);
      stream->strides[least].stride = stride;
      stream->strides[least].count++;
   }
}

static Long addr_delta(Addr current, Addr previous)
{
   if (current >= previous) {
      return (Long)(current - previous);
   }
   return -(Long)(previous - current);
}

static void update_stream(AccessStream* stream, Addr addr, SizeT size)
{
   stream->accesses++;

   if (!stream->have_prev) {
      stream->prev_addr = addr;
      stream->have_prev = True;
      return;
   }

   {
      Long stride = addr_delta(addr, stream->prev_addr);

      stream->stride_samples++;
      if (stride == (Long)size || stride == -(Long)size) {
         stream->sequential_steps++;
      }
      add_stride_sample(stream, stride);
      stream->prev_addr = addr;
   }
}

static Bool regular_enough(ULong part, ULong total)
{
   return part * 100 >= (ULong)clo_sequential_threshold * total;
}

static void finish_stream(AccessStream* stream)
{
   UInt i;
   ULong dominant_count = 0;

   stream->pattern = Pattern_None;
   stream->dominant_stride = 0;

   if (stream->stride_samples == 0) {
      return;
   }

   for (i = 0; i < stream->n_strides; i++) {
      if (stream->strides[i].count > dominant_count) {
         dominant_count = stream->strides[i].count;
         stream->dominant_stride = stream->strides[i].stride;
      }
   }

   if (regular_enough(stream->sequential_steps, stream->stride_samples)) {
      stream->pattern = Pattern_Sequential;
   } else if (regular_enough(dominant_count, stream->stride_samples)) {
      stream->pattern = Pattern_Strided;
   } else {
      stream->pattern = Pattern_Random;
   }
}

static void finish_all_streams(void)
{
   VG_(OSetGen_ResetIter)(functions);
   while (True) {
      FunctionStats* stats = VG_(OSetGen_Next)(functions);
      if (stats == NULL) {
         break;
      }
      finish_stream(&stats->reads);
      finish_stream(&stats->writes);
   }
}

static const HChar* pattern_name(AccessPattern pattern)
{
   switch (pattern) {
      case Pattern_None:       return "n/a";
      case Pattern_Sequential: return "sequential";
      case Pattern_Strided:    return "strided";
      case Pattern_Random:     return "random";
      default:                 return "n/a";
   }
}

static VG_REGPARM(3)
void record_read(FunctionStats* stats, Addr addr, SizeT size)
{
   update_stream(&stats->reads, addr, size);
}

static VG_REGPARM(3)
void record_write(FunctionStats* stats, Addr addr, SizeT size)
{
   update_stream(&stats->writes, addr, size);
}

/*------------------------------------------------------------*/
/*--- Instrumentation                                      ---*/
/*------------------------------------------------------------*/

static void instrument_access(IRSB* sbOut,
                              FunctionStats* stats,
                              IRExpr* addr,
                              Int size,
                              Bool is_write,
                              IRExpr* guard)
{
   IRExpr** argv;
   IRDirty* di;

   tl_assert(isIRAtom(addr));
   tl_assert(size > 0);

   argv = mkIRExprVec_3(
             mkIRExpr_HWord((HWord)stats),
             addr,
             mkIRExpr_HWord((HWord)size)
          );
   if (is_write) {
      di = unsafeIRDirty_0_N(
              3, "record_write",
              VG_(fnptr_to_fnentry)(&record_write),
              argv
           );
   } else {
      di = unsafeIRDirty_0_N(
              3, "record_read",
              VG_(fnptr_to_fnentry)(&record_read),
              argv
           );
   }
   if (guard != NULL) {
      di->guard = guard;
   }
   addStmtToIRSB(sbOut, IRStmt_Dirty(di));
}

static void instrument_load(IRSB* sbOut,
                            FunctionStats* stats,
                            IRExpr* addr,
                            Int size,
                            IRExpr* guard)
{
   instrument_access(sbOut, stats, addr, size, False, guard);
}

static void instrument_store(IRSB* sbOut,
                             FunctionStats* stats,
                             IRExpr* addr,
                             Int size,
                             IRExpr* guard)
{
   instrument_access(sbOut, stats, addr, size, True, guard);
}

static IRSB* ma_instrument(VgCallbackClosure* closure,
                           IRSB* sbIn,
                           const VexGuestLayout* layout,
                           const VexGuestExtents* vge,
                           const VexArchInfo* archinfo_host,
                           IRType gWordTy,
                           IRType hWordTy)
{
   IRSB* sbOut;
   IRTypeEnv* tyenv;
   FunctionStats* current_stats;
   Int i;

   if (gWordTy != hWordTy) {
      VG_(tool_panic)("host/guest word size mismatch");
   }

   sbOut = deepCopyIRSBExceptStmts(sbIn);
   tyenv = sbIn->tyenv;
   current_stats = unknown_function_stats();

   i = 0;
   while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB(sbOut, sbIn->stmts[i]);
      i++;
   }

   for (; i < sbIn->stmts_used; i++) {
      IRStmt* st = sbIn->stmts[i];

      if (st == NULL || st->tag == Ist_NoOp) {
         continue;
      }

      switch (st->tag) {
         case Ist_AbiHint:
         case Ist_Put:
         case Ist_PutI:
         case Ist_MBE:
            addStmtToIRSB(sbOut, st);
            break;

         case Ist_IMark:
            current_stats = get_function_stats(st->Ist.IMark.addr);
            addStmtToIRSB(sbOut, st);
            break;

         case Ist_WrTmp: {
            IRExpr* expr = st->Ist.WrTmp.data;
            if (expr->tag == Iex_Load) {
               instrument_load(sbOut, current_stats,
                               expr->Iex.Load.addr,
                               sizeofIRType(expr->Iex.Load.ty),
                               NULL);
            }
            addStmtToIRSB(sbOut, st);
            break;
         }

         case Ist_Store: {
            IRType type = typeOfIRExpr(tyenv, st->Ist.Store.data);
            instrument_store(sbOut, current_stats,
                             st->Ist.Store.addr,
                             sizeofIRType(type),
                             NULL);
            addStmtToIRSB(sbOut, st);
            break;
         }

         case Ist_StoreG: {
            IRStoreG* store = st->Ist.StoreG.details;
            IRType type = typeOfIRExpr(tyenv, store->data);
            instrument_store(sbOut, current_stats,
                             store->addr,
                             sizeofIRType(type),
                             store->guard);
            addStmtToIRSB(sbOut, st);
            break;
         }

         case Ist_LoadG: {
            IRLoadG* load = st->Ist.LoadG.details;
            IRType type = Ity_INVALID;
            IRType wide_type = Ity_INVALID;
            typeOfIRLoadGOp(load->cvt, &wide_type, &type);
            instrument_load(sbOut, current_stats,
                            load->addr,
                            sizeofIRType(type),
                            load->guard);
            addStmtToIRSB(sbOut, st);
            break;
         }

         case Ist_Dirty: {
            IRDirty* dirty = st->Ist.Dirty.details;
            if (dirty->mFx == Ifx_Read || dirty->mFx == Ifx_Modify) {
               instrument_load(sbOut, current_stats,
                               dirty->mAddr, dirty->mSize, dirty->guard);
            }
            if (dirty->mFx == Ifx_Write || dirty->mFx == Ifx_Modify) {
               instrument_store(sbOut, current_stats,
                                dirty->mAddr, dirty->mSize, dirty->guard);
            }
            addStmtToIRSB(sbOut, st);
            break;
         }

         case Ist_CAS: {
            IRCAS* cas = st->Ist.CAS.details;
            IRType type = typeOfIRExpr(tyenv, cas->dataLo);
            Int size = sizeofIRType(type);
            if (cas->dataHi != NULL) {
               size *= 2;
            }
            instrument_load(sbOut, current_stats,
                            cas->addr, size, NULL);
            instrument_store(sbOut, current_stats,
                             cas->addr, size, NULL);
            addStmtToIRSB(sbOut, st);
            break;
         }

         case Ist_LLSC: {
            IRType type;
            if (st->Ist.LLSC.storedata == NULL) {
               type = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               instrument_load(sbOut, current_stats,
                               st->Ist.LLSC.addr,
                               sizeofIRType(type),
                               NULL);
            } else {
               type = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               instrument_store(sbOut, current_stats,
                                st->Ist.LLSC.addr,
                                sizeofIRType(type),
                                NULL);
            }
            addStmtToIRSB(sbOut, st);
            break;
         }

         case Ist_Exit:
            addStmtToIRSB(sbOut, st);
            break;

         default:
            ppIRStmt(st);
            tl_assert(0);
      }
   }

   return sbOut;
}

/*------------------------------------------------------------*/
/*--- Output                                               ---*/
/*------------------------------------------------------------*/

static void write_flamegraph_name(VgFile* fp, const HChar* name)
{
   const HChar* p;

   for (p = name; *p != '\0'; p++) {
      HChar c = *p;
      if (c == ';' || c == ' ' || c == '\t' ||
          c == '\n' || c == '\r') {
         VG_(fprintf)(fp, "_");
      } else {
         VG_(fprintf)(fp, "%c", c);
      }
   }
}

static void write_flamegraph_stream(VgFile* fp,
                                    const HChar* function_name,
                                    const HChar* operation,
                                    const AccessStream* stream)
{
   if (stream->accesses == 0) {
      return;
   }

   VG_(fprintf)(fp, "memaccess;%s;", operation);
   write_flamegraph_name(fp, function_name);
   VG_(fprintf)(fp, ";%s %llu\n",
                pattern_name(stream->pattern),
                (unsigned long long)stream->accesses);
}

static void write_flamegraph_file(void)
{
   HChar* path = VG_(expand_file_name)("--flamegraph-out-file",
                                       clo_flamegraph_out_file);
   VgFile* fp = VG_(fopen)(
                   path,
                   VKI_O_CREAT | VKI_O_TRUNC | VKI_O_WRONLY,
                   VKI_S_IRUSR | VKI_S_IWUSR
                );

   if (fp == NULL) {
      VG_(umsg)("MemAccess: cannot open flame graph output file '%s'\n", path);
      VG_(free)(path);
      return;
   }

   VG_(OSetGen_ResetIter)(functions);
   while (True) {
      FunctionStats* stats = VG_(OSetGen_Next)(functions);
      if (stats == NULL) {
         break;
      }

      write_flamegraph_stream(fp, stats->name, "READ", &stats->reads);
      write_flamegraph_stream(fp, stats->name, "WRITE", &stats->writes);
   }

   VG_(fclose)(fp);

   if (VG_(clo_verbosity) > 0) {
      VG_(umsg)("MemAccess: folded flame graph data written to %s\n", path);
   }
   VG_(free)(path);
}

static void print_summary(void)
{
   HChar read_stride[32];
   HChar write_stride[32];

   VG_(umsg)("\n");
   VG_(umsg)("%-32s %12s %12s %10s %-11s %10s %-11s\n",
             "Function", "Reads", "Writes",
             "R-stride", "R-pattern", "W-stride", "W-pattern");

   VG_(OSetGen_ResetIter)(functions);
   while (True) {
      FunctionStats* stats = VG_(OSetGen_Next)(functions);
      if (stats == NULL) {
         break;
      }

      if (stats->reads.accesses == 0 && stats->writes.accesses == 0) {
         continue;
      }

      if (stats->reads.stride_samples == 0) {
         VG_(strcpy)(read_stride, "-");
      } else {
         VG_(snprintf)(read_stride, sizeof(read_stride), "%lld",
                       (long long)stats->reads.dominant_stride);
      }
      if (stats->writes.stride_samples == 0) {
         VG_(strcpy)(write_stride, "-");
      } else {
         VG_(snprintf)(write_stride, sizeof(write_stride), "%lld",
                       (long long)stats->writes.dominant_stride);
      }

      VG_(umsg)("%-32s %'12llu %'12llu %10s %-11s %10s %-11s\n",
                stats->name,
                (unsigned long long)stats->reads.accesses,
                (unsigned long long)stats->writes.accesses,
                read_stride, pattern_name(stats->reads.pattern),
                write_stride, pattern_name(stats->writes.pattern));
   }
}

static void ma_fini(Int exitcode)
{
   finish_all_streams();
   print_summary();
   write_flamegraph_file();
   (void)exitcode;
}

static void ma_post_clo_init(void)
{
   functions = VG_(OSetGen_Create)(
                  offsetof(FunctionStats, name),
                  function_cmp,
                  VG_(malloc), "memaccess.functions",
                  VG_(free)
               );
}

static void ma_pre_clo_init(void)
{
   VG_(details_name)("MemAccess");
   VG_(details_version)(NULL);
   VG_(details_description)("a memory access pattern profiler");
   VG_(details_copyright_author)(
      "Copyright (C) 2026, and GNU GPL'd, by the Valgrind developers.");
   VG_(details_bug_reports_to)(VG_BUGS_TO);
   VG_(details_avg_translation_sizeB)(400);

   VG_(basic_tool_funcs)(ma_post_clo_init,
                         ma_instrument,
                         ma_fini);
   VG_(needs_command_line_options)(ma_process_cmd_line_option,
                                   ma_print_usage,
                                   ma_print_debug_usage);
}

VG_DETERMINE_INTERFACE_VERSION(ma_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                ma_main.c ---*/
/*--------------------------------------------------------------------*/
