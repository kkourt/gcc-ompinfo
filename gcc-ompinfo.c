/**
 * GCC plugin to draw a graph of OpenMP constructs using graphviz
 * Kornilios Kourtis <kkourt@cslab.ece.ntua.gr>
 */
#include <stdlib.h>

#include <gcc-plugin.h>
#include <tree-pass.h>
#include <gimple.h>
#include <splay-tree.h>
#include <tree.h>         /* build_function_type, build_fn_decl */
#include <tree-flow.h>
#include <pretty-print.h>
#include <cgraph.h>

/* graphviz uses a cache for strings (e.g., names, labels), and will allocate
 * memory as needed by itself -- i.e., we can just use the stack for strings
 * passsed to it */
#include <graphviz/gvc.h>

/* Global variables for loading the graph */
static Agraph_t *gvGraph     = NULL;
static char     *gvOutfile   = NULL;
//static GVC_t    *gvCtx       = NULL;

/* counter of OMP constructs, used to assigne unique ids */
static int omp_constructs_cnt = 1;

/* Use a splay tree to maintain the parent for each subgraph.
 * (Tried to use the graphviz structure, but I don't think that there is a way)
 */
static splay_tree subgraph_parents;

/* declare needed functions that are not in plugin headers */
extern int dump_generic_node (pretty_printer *, tree, int, int, bool); /* tree-pretty-print.h */

static Agraph_t *
new_subgraph(Agraph_t *parent, char *name)
{
	Agraph_t *g;

	/* create a new subgraph, and update subgraph_parents */
	g = agsubg(parent, name);
	splay_tree_insert(subgraph_parents, (splay_tree_key)g, (splay_tree_value)parent);
	return g;
}

static void
do_agset(void *obj, char *attr, char *value)
{
	if (agset(obj, attr, value) == -1)
		fprintf(stderr, "Couldn't set [%s,%s] to %p\n", attr,value,obj);
}

/* create a subgraph for a new function */
static Agraph_t *
mk_function_subgraph(Agraph_t *parent)
{
	const char *fn_name = current_function_name();
	char subgraph_name[strlen(fn_name) + 12];
	Agraph_t *g;

	assert(parent == gvGraph);
	snprintf(subgraph_name, sizeof(subgraph_name), "cluster_%s", fn_name);

	g = new_subgraph(parent, subgraph_name);

	do_agset(g, "style", "solid");
	do_agset(g, "color", "blue");

	return g;
}

/* create an entry node for a function */
static Agnode_t *
mk_entry_node(Agraph_t *graph)
{
	const char *fn_name = current_function_name();
	char node_name[strlen(fn_name) + 128];
	Agnode_t *n;

	snprintf(node_name, sizeof(node_name), "%s()", fn_name);
	n = agnode(graph, node_name);
	do_agset(n, "shape", "note");

	return n;
}

/* create a subgraph for an OMP construct */
static Agraph_t *
mk_omp_subgraph(Agraph_t *parent, const char *omp_name, int next_id)
{
	Agraph_t *g;
	int omp_len = strlen(omp_name);
	int gimple_len = 7; // strlen("gimple_");
	assert(omp_len > gimple_len);
	char subgraph_name[omp_len - gimple_len + 40];

	/* name it cluster_*, so that graphviz can draw it properly */
	snprintf(subgraph_name, sizeof(subgraph_name), "cluster_%s.%d", omp_name + gimple_len, next_id);

	g = new_subgraph(parent, subgraph_name);

	do_agset(g, "style", "solid");
	do_agset(g, "color", "lightgray");

	return g;
}

static Agnode_t *
mk_omp_node(Agraph_t *graph, const char *omp_name, int next_id, Agnode_t *parent_node)
{
	Agnode_t *n;
	int omp_len = strlen(omp_name);
	int gimple_len = 7; // strlen("gimple_");
	assert(omp_len > gimple_len);
	char node_name[omp_len - gimple_len + 32];

	snprintf(node_name, sizeof(node_name), "%s.%d", omp_name + gimple_len, next_id);
	n = agnode(graph, node_name);
	do_agset(n, "shape", "box");

	if (parent_node)
		agedge(gvGraph, parent_node, n);

	return n;
}


/*
 * Create a label for an OMP for construct
 *
 * references:
 * gimple-pretty-print.c:dump_gimple_omp_for()
 * omp_low.c:extract_omp_for_data()
 */
static void
mk_omp_for_label(Agnode_t *node, gimple stmt, const char *omp_name)
{
	int  i;
	pretty_printer pp;
	pp_construct(&pp, NULL, 1024);

	/* use a quick hack to use dump_generic_node():
	 *  - create a pipe, and use it as a file descriptor
	 *  - read the data from the pipe to a buffer
	 *  - use the buffer for the label
	 */
	int pipefd[2], ret;
	FILE *pipefp_rd, *pipefp_wr;
	ret = pipe(pipefd);
	if (ret < 0){
		perror("pipe");
		exit(1);
	}
	pipefp_rd = fdopen(pipefd[0], "r");
	pipefp_wr = fdopen(pipefd[1], "w");
	if (pipefp_rd == NULL || pipefp_wr == NULL) {
		perror("fdopen");
		exit(1);
	}
	pp.buffer->stream = pipefp_wr;

	/* wrapper for dump_generic_node() */
	void dump_n(tree _n) {
		dump_generic_node(&pp, _n, 0, 0, false);
	}

	pp_printf(&pp, "{ %s", omp_name);
	for (i=0; i<gimple_omp_for_collapse(stmt); i++) {
		pp_string(&pp, " | for (");
		/* initial */
		dump_n(gimple_omp_for_index(stmt, i));
		pp_string(&pp, "=");
		dump_n(gimple_omp_for_initial(stmt, i));
		pp_string(&pp, "; ");
		/* condition */
		dump_n(gimple_omp_for_index(stmt, i));
		switch (gimple_omp_for_cond (stmt, i))
		{
			case LT_EXPR:
			pp_string(&pp, "\\<");
			break;

			case GT_EXPR:
			pp_string(&pp, "\\>");
			break;

			case LE_EXPR:
			pp_string (&pp, "\\<=");
			break;

			case GE_EXPR:
			pp_string (&pp, "\\>=");
			break;

			default:
			assert(false);
		}
		dump_n(gimple_omp_for_final(stmt, i));
		pp_string(&pp, "; ");
		/* increment */
		dump_n(gimple_omp_for_index(stmt, i));
		pp_string(&pp, " = ");
		dump_n(gimple_omp_for_incr(stmt, i));
		pp_string(&pp, ")");
	}
	pp_string(&pp, "}");
	pp_flush(&pp);
	fclose(pipefp_wr);

	/* read data from the label, and use them as label */
	char label[1024];
	ret = fread(label, 1, sizeof(label), pipefp_rd);
	assert(ret < sizeof(label) - 1);
	label[ret - 1] = '\0'; // last one is a newline
	do_agset(node, "label", label);
}

/* create an OMP for node */
static Agnode_t *
mk_omp_for_node(gimple omp_for, Agraph_t *graph, int next_id, Agnode_t *parent_node)
{
	Agnode_t *n;
	char node_name[16];

	snprintf(node_name, sizeof(node_name), "omp_for.%d", next_id);
	n = agnode(graph, node_name);
	do_agset(n, "shape", "record");
	mk_omp_for_label(n, omp_for, node_name);

	if (parent_node)
		agedge(gvGraph, parent_node, n);

	return n;
}

/**
 * Recursively called function to generate the graph of the OMP structues
 * based on: gcc/omp-low.c:build_omp_regions_1()
 */
static void
do_omp_graph(basic_block bb, int parent_id, Agnode_t *parent_node, Agraph_t *parent_graph)
{
	int next_id              = parent_id;
	Agnode_t *next_node      = parent_node;
	gimple_stmt_iterator gsi = gsi_last_bb(bb);
	gimple stmt;

	//gimple_dump_bb(bb, stdout, 1, 0);

	/* OMP statements should be the last on the basic block */
	if (!gsi_end_p(gsi) && is_gimple_omp(stmt = gsi_stmt(gsi)) ) {

		/* If this is the first OMP construct in the function, create
		 * an entry node for the function. This node will be the
		 * parent graph */
		if (parent_node == NULL) {
			parent_graph = mk_function_subgraph(parent_graph);
			parent_node = mk_entry_node(parent_graph);
		}

		enum gimple_code code = gimple_code(stmt);
		const char *omp_name = gimple_code_name[code];
		next_id = omp_constructs_cnt++;
		switch (code) {
			/* Create an OMP node, and a subgraph.
			 * Basically, everything that pairs with a return, and
			 * does not need special treatment (i.e., an OMP for)
			 * should go here */
			case GIMPLE_OMP_TASK:
			case GIMPLE_OMP_CRITICAL:
			case GIMPLE_OMP_SINGLE:
			case GIMPLE_OMP_PARALLEL:
			parent_graph = mk_omp_subgraph(parent_graph, omp_name, next_id);
			next_node = mk_omp_node(parent_graph, omp_name, next_id, parent_node);
			break;

			/* create a node and a subgraph for an OMP for */
			case GIMPLE_OMP_FOR:
			parent_graph = mk_omp_subgraph(parent_graph, omp_name, next_id);
			next_node = mk_omp_for_node(stmt, parent_graph, next_id, parent_node);
			break;

			/* pop a subgraph */
			case GIMPLE_OMP_RETURN: {
				splay_tree_node n;
				n = splay_tree_lookup(subgraph_parents, (splay_tree_key)parent_graph);
				assert(n != NULL);
				parent_graph = (Agraph_t *)n->value;
				next_node = mk_omp_node(parent_graph, omp_name, next_id, parent_node);
			}
			break;

			/* XXX: some of these might pair with OMP_RETURN, so they should move up */
			case GIMPLE_OMP_MASTER:
			case GIMPLE_OMP_ORDERED:
			case GIMPLE_OMP_SECTION:
			case GIMPLE_OMP_SECTIONS:
			case GIMPLE_OMP_SECTIONS_SWITCH:
			case GIMPLE_OMP_ATOMIC_LOAD:
			case GIMPLE_OMP_ATOMIC_STORE:
			case GIMPLE_OMP_CONTINUE:
			next_node = mk_omp_node(parent_graph, omp_name, next_id, parent_node);
			break;

			default:
			/* should not reach here */
			fprintf(stderr, "Unknown OMP code!\n");
			gcc_unreachable();
		}
	}

	/* recursively handle children */
	basic_block son = first_dom_son(CDI_DOMINATORS, bb);
	while (son) {
		do_omp_graph(son, next_id, next_node, parent_graph);
		son = next_dom_son(CDI_DOMINATORS, son);
	}
}

/**
 * Create the graph of the OMP structures.
 *  based on gcc/omp-low.c:build_omp_regions()
 */
static unsigned int
omp_graph_execute(void)
{
	printf("\t%s: %s\n", __FUNCTION__, current_function_name());
	calculate_dominance_info(CDI_DOMINATORS);
	do_omp_graph(ENTRY_BLOCK_PTR, omp_constructs_cnt, NULL, gvGraph);

	return 0;
}

static bool
omp_pass_gate(void)
{
	/* just use pas_expand_omp's gate */
	return pass_expand_omp.pass.gate();
}

static struct gimple_opt_pass omp_graph_pass = {{
	.type                 = GIMPLE_PASS,
	.name                 = "*omp_graph",
	.gate                 = omp_pass_gate,
	.execute              = omp_graph_execute,
	.sub                  = NULL,
	.next                 = NULL,
	.static_pass_number   = 0,
	.tv_id                = TV_NONE,
	.properties_required  = PROP_gimple_any,
	.properties_provided  = 0,
	.properties_destroyed = 0,
	.todo_flags_start     = 0,
	.todo_flags_finish    = 0
}};

static struct register_pass_info omp_graph_info = {
	.pass                     = &omp_graph_pass.pass,
	.reference_pass_name      = "ompexp",
	.ref_pass_instance_number = 0,
	.pos_op                   = PASS_POS_INSERT_BEFORE
};

int plugin_is_GPL_compatible;

static void
plugin_finish(void *gcc_data, void *user_data)
{
	assert(gcc_data == NULL && user_data == NULL);
	printf("%s:%s\n", __FILE__, __FUNCTION__);

	char filename[strlen(gvOutfile) + 8];
	snprintf(filename, sizeof(filename), "%s.dot", gvOutfile);

	/** UberBUG:
	 * The code below was commented because gvRenderFilename() uses a
	 * emit_label() global function, which is also a global function of
	 * gcc. The gcc function is called, and all hell breaks loose.
	 *
	 * To avoid other hidden bugs like this, we do minimum use of the
	 * graphviz library functions (just libgraph). Rendering, etc, can
	 * be done externally.
	 *
	 * gvLayout(gvCtx, gvGraph, "dot");
	 * gvRenderFilename(gvCtx, gvGraph, "dot", filename);
	 * gvFreeLayout(gvCtx, gvGraph);
	 * gvFreeContext(gvCtx);
	 */
	FILE *filep;
	filep = fopen(filename, "w");
	if (filep == NULL){
		perror(filename);
		exit(1);
	}
	agwrite(gvGraph, filep);
	fclose(filep);

	agclose(gvGraph);
	splay_tree_delete(subgraph_parents);
	//dump_cgraph(stdout);
}

int
plugin_init(struct plugin_name_args   *plugin_info,
            struct plugin_gcc_version *version)
{
	int argc, i;
	struct plugin_argument *argv;

	printf("%s:%s\n", __FILE__, __FUNCTION__);
	argc = plugin_info->argc;
	argv = plugin_info->argv;

	gvOutfile = "output";
	printf("\tplugin arguments:[");
	for (i=0; i<argc; i++) {
		if (strcmp(argv[i].key, "output") == 0) {
			gvOutfile = argv[i].value;
			printf("%s=%s ", argv[i].key, argv[i].value);
		} else {
			printf("%s=%s:IGNORED! ", argv[i].key, argv[i].value);
		}
	}
	printf("]\n");

	register_callback(plugin_info->base_name,
	                  PLUGIN_PASS_MANAGER_SETUP,
	                  NULL,
	                  &omp_graph_info);

	register_callback(plugin_info->base_name,
	                  PLUGIN_FINISH,
			  plugin_finish,
			  NULL);

	/* initialize graphviz */
	//gvCtx = gvContext();
	aginit(); /* normally called by gvContext() */
	//agnodeattr(NULL, "label", "\\N"); /* ditto */

	/* we need to set default values for attributes we want to customize */
	agnodeattr(NULL, "shape", "oval");
	agraphattr(NULL, "style", "invis");
	agraphattr(NULL, "color", "black");
	agnodeattr(NULL, "label", "");
	agnodeattr(NULL, "rankdir", "");

	gvGraph = agopen(gvOutfile, AGDIGRAPHSTRICT);
	subgraph_parents = splay_tree_new(splay_tree_compare_pointers, 0, 0);

	return 0;
}
