/*
 * jit-block.c - Functions for manipulating blocks.
 *
 * Copyright (C) 2004  Southern Storm Software, Pty Ltd.
 *
 * This file is part of the libjit library.
 *
 * The libjit library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * The libjit library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the libjit library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "jit-internal.h"
#include "jit-memory.h"

/*@

@cindex jit-block.h

@*/

/* helper data structure for CFG DFS traversal */
typedef struct _jit_block_stack_entry
{
	jit_block_t block;
	int index;
} _jit_block_stack_entry_t;


static void
create_edge(jit_function_t func, jit_block_t src, jit_block_t dst, int flags, int create)
{
	_jit_edge_t edge;

	/* Create edge if required */
	if(create)
	{
		/* Allocate memory for it */
		edge = jit_memory_pool_alloc(&func->builder->edge_pool, struct _jit_edge);
		if(!edge)
		{
			jit_exception_builtin(JIT_RESULT_OUT_OF_MEMORY);
		}

		/* Initialize edge fields */
		edge->src = src;
		edge->dst = dst;
		edge->flags = flags;

		/* Store edge pointers in source and destination nodes */
		src->succs[src->num_succs] = edge;
		dst->preds[dst->num_preds] = edge;
	}

	/* Count it */
	++(src->num_succs);
	++(dst->num_preds);
}

static void
build_edges(jit_function_t func, int create)
{
	jit_block_t src, dst;
	jit_insn_t insn;
	int opcode, flags;
	jit_label_t *labels;
	int index, num_labels;

	/* TODO: Handle catch, finally, filter blocks. */

	for(src = func->builder->entry_block; src != func->builder->exit_block; src = src->next)
	{
		/* Check the last instruction of the block */
		insn = _jit_block_get_last(src);
		opcode = insn ? insn->opcode : JIT_OP_NOP;
		if(opcode >= JIT_OP_RETURN && opcode <= JIT_OP_RETURN_SMALL_STRUCT)
		{
			flags = _JIT_EDGE_RETURN;
			dst = func->builder->exit_block;
		}
		else if(opcode == JIT_OP_BR)
		{
			flags = _JIT_EDGE_BRANCH;
			dst = jit_block_from_label(func, (jit_label_t) insn->dest);
			if(!dst)
			{
				/* Bail out on undefined label */
				jit_exception_builtin(JIT_RESULT_UNDEFINED_LABEL);
			}
		}
		else if(opcode > JIT_OP_BR && opcode <= JIT_OP_BR_NFGE_INV)
		{
			flags = _JIT_EDGE_BRANCH;
			dst = jit_block_from_label(func, (jit_label_t) insn->dest);
			if(!dst)
			{
				/* Bail out on undefined label */
				jit_exception_builtin(JIT_RESULT_UNDEFINED_LABEL);
			}
		}
		else if(opcode == JIT_OP_THROW || opcode == JIT_OP_RETHROW)
		{
			flags = _JIT_EDGE_EXCEPT;
			dst = jit_block_from_label(func, func->builder->catcher_label);
			if(!dst)
			{
				dst = func->builder->exit_block;
			}
		}
		else if(opcode == JIT_OP_CALL_FINALLY || opcode == JIT_OP_CALL_FILTER)
		{
			flags = _JIT_EDGE_EXCEPT;
			dst = jit_block_from_label(func, (jit_label_t) insn->dest);
			if(!dst)
			{
				/* Bail out on undefined label */
				jit_exception_builtin(JIT_RESULT_UNDEFINED_LABEL);
			}
		}
		else if(opcode >= JIT_OP_CALL && opcode <= JIT_OP_CALL_EXTERNAL_TAIL)
		{
			flags = _JIT_EDGE_EXCEPT;
			dst = jit_block_from_label(func, func->builder->catcher_label);
			if(!dst)
			{
				dst = func->builder->exit_block;
			}
		}
		else if(opcode == JIT_OP_JUMP_TABLE)
		{
			labels = (jit_label_t *) insn->value1->address;
			num_labels = (int) insn->value2->address;
			for(index = 0; index < num_labels; index++)
			{
				dst = jit_block_from_label(func, labels[index]);
				if(!dst)
				{
					/* Bail out on undefined label */
					jit_exception_builtin(JIT_RESULT_UNDEFINED_LABEL);
				}
				create_edge(func, src, dst, _JIT_EDGE_BRANCH, create);
			}
			dst = 0;
		}
		else
		{
			dst = 0;
		}

		/* create a branch or exception edge if appropriate */
		if(dst)
		{
			create_edge(func, src, dst, flags, create);
		}
		/* create a fall-through edge if appropriate */
		if(!src->ends_in_dead)
		{
			create_edge(func, src, src->next, _JIT_EDGE_FALLTHRU, create);
		}
	}
}

static void
alloc_edges(jit_function_t func)
{
	jit_block_t block;

	for(block = func->builder->entry_block; block; block = block->next)
	{
		/* Allocate edges to successor nodes */
		if(block->num_succs == 0)
		{
			block->succs = 0;
		}
		else
		{
			block->succs = jit_calloc(block->num_succs, sizeof(_jit_edge_t));
			if(!block->succs)
			{
				jit_exception_builtin(JIT_RESULT_OUT_OF_MEMORY);
			}
			/* Reset edge count for the next build pass */
			block->num_succs = 0;
		}

		/* Allocate edges to predecessor nodes */
		if(block->num_preds == 0)
		{
			block->preds = 0;
		}
		else
		{
			block->preds = jit_calloc(block->num_preds, sizeof(_jit_edge_t));
			if(!block->preds)
			{
				jit_exception_builtin(JIT_RESULT_OUT_OF_MEMORY);
			}
			/* Reset edge count for the next build pass */
			block->num_preds = 0;
		}
	}
}

static void
detach_edge_src(_jit_edge_t edge)
{
	jit_block_t block;
	int index;

	block = edge->src;
	for(index = 0; index < block->num_succs; index++)
	{
		if(block->succs[index] == edge)
		{
			for(block->num_succs--; index < block->num_succs; index++)
			{
				block->succs[index] = block->succs[index + 1];
			}
			block->succs = jit_realloc(block->succs, block->num_succs * sizeof(_jit_edge_t));
			return;
		}
	}
}

static void
detach_edge_dst(_jit_edge_t edge)
{
	jit_block_t block;
	int index;

	block = edge->dst;
	for(index = 0; index < block->num_preds; index++)
	{
		if(block->preds[index] == edge)
		{
			for(block->num_preds--; index < block->num_preds; index++)
			{
				block->preds[index] = block->preds[index + 1];
			}
			block->preds = jit_realloc(block->preds,
						   block->num_preds * sizeof(_jit_edge_t));
			return;
		}
	}
}

static int
attach_edge_dst(_jit_edge_t edge, jit_block_t block)
{
	_jit_edge_t *preds;

	preds = jit_realloc(block->preds, (block->num_preds + 1) * sizeof(_jit_edge_t));
	if(!preds)
	{
		return 0;
	}

	preds[block->num_preds++] = edge;
	block->preds = preds;
	edge->dst = block;

	return 1;
}

/* Delete edge along with references to it */
static void
delete_edge(jit_function_t func, _jit_edge_t edge)
{
	detach_edge_src(edge);
	detach_edge_dst(edge);
	jit_memory_pool_dealloc(&func->builder->edge_pool, edge);
}

/* Block may not be deleted right when it was found useless from
   the control flow perspective as it might be referenced from
   elsewhere, for instance, from some jit_value_t */
static void
delete_block(jit_block_t block)
{
	jit_free(block->succs);
	block->succs = 0;
	jit_free(block->preds);
	block->preds = 0;
	jit_free(block->insns);
	block->insns = 0;

	block->next = block->func->builder->deleted_blocks;
	block->func->builder->deleted_blocks = block->next;
}

/* The block is empty if it contains nothing apart from an unconditional branch */
static int
is_empty_block(jit_block_t block)
{
	int index, opcode;

	for(index = 0; index < block->num_insns; index++)
	{
		opcode = block->insns[index].opcode;
		if(opcode != JIT_OP_NOP
		   && opcode != JIT_OP_MARK_OFFSET
		   && opcode != JIT_OP_BR)
		{
			return 0;
		}
	}

	return 1;
}

static void
merge_labels(jit_function_t func, jit_block_t block, jit_label_t label)
{
	_jit_label_info_t *info;
	jit_label_t alias;

	while(label != jit_label_undefined)
	{
		info = &func->builder->label_info[label];
		alias = info->alias;
		info->block = block;
		info->alias = block->label;
		block->label = label;
		label = alias;
	}
}

/* Merge empty block with its successor */
static void
merge_empty(jit_function_t func, jit_block_t block, int *changed)
{
	_jit_edge_t succ_edge, pred_edge, fallthru_edge;
	jit_block_t succ_block;
	int index;

	/* Find block successor */
	succ_edge = block->succs[0];
	succ_block = succ_edge->dst;

	/* Retarget labels bound to this block to the successor block. */
	merge_labels(func, succ_block, block->label);

	/* Retarget all incoming edges except a fallthrough edge */
	fallthru_edge = 0;
	for(index = 0; index < block->num_preds; index++)
	{
		pred_edge = block->preds[index];
		if(pred_edge->flags == _JIT_EDGE_FALLTHRU)
		{
			fallthru_edge = pred_edge;
		}
		else
		{
			*changed = 1;
			if(!attach_edge_dst(pred_edge, succ_block))
			{
				jit_exception_builtin(JIT_RESULT_OUT_OF_MEMORY);
			}
		}
	}

	/* If there is an incoming fallthrough edge then retarget it
	   if the outgoing edge is also fallthough. Otherwise adjust
	   the preds array to contain this edge only.  */
	if(fallthru_edge != NULL)
	{
		if(succ_edge->flags == _JIT_EDGE_FALLTHRU)
		{
			*changed = 1;
			if(!attach_edge_dst(pred_edge, succ_block))
			{
				jit_exception_builtin(JIT_RESULT_OUT_OF_MEMORY);
			}
			fallthru_edge = 0;
		}
		else if (block->num_preds > 1)
		{
			block->num_preds = 1;
			block->preds = jit_realloc(block->preds, sizeof(_jit_edge_t));
			block->preds[0] = fallthru_edge;
		}
	}

	/* Free block if no incoming edge is left */
	if(!fallthru_edge)
	{
		detach_edge_dst(succ_edge);
		jit_memory_pool_dealloc(&func->builder->edge_pool, succ_edge);
		_jit_block_detach(block, block);
		delete_block(block);
	}
}

/* Delete block along with references to it */
static void
eliminate_block(jit_block_t block)
{
	_jit_edge_t edge;
	int index;

	/* Detach block from the list */
	_jit_block_detach(block, block);

	/* Remove control flow graph edges */
	for(index = 0; index < block->num_succs; index++)
	{
		edge = block->succs[index];
		detach_edge_dst(edge);
		jit_memory_pool_dealloc(&block->func->builder->edge_pool, edge);
	}
	for(index = 0; index < block->num_preds; index++)
	{
		edge = block->preds[index];
		detach_edge_src(edge);
		jit_memory_pool_dealloc(&block->func->builder->edge_pool, edge);
	}

	/* Finally delete the block */
	delete_block(block);
}

#if 0

/* Visit all successive blocks recursively */
static void
visit_reachable(jit_block_t block)
{
	int index;

	if(!block->visited)
	{
		block->visited = 1;
		for(index = 0; index < block->num_succs; index++)
		{
			visit_reachable(block->succs[index]->dst);
		}
	}
}

#endif

/* Eliminate unreachable blocks */
static void
eliminate_unreachable(jit_function_t func)
{
	jit_block_t block, next_block;

	block = func->builder->entry_block;
	while(block != func->builder->exit_block)
	{
		next_block = block->next;
		if(block->visited)
		{
			block->visited = 0;
		}
		else
		{
			eliminate_block(block);
		}
		block = next_block;
	}
}

/* Clear visited blocks */
static void
clear_visited(jit_function_t func)
{
	jit_block_t block;

	for(block = func->builder->entry_block; block; block = block->next)
	{
		block->visited = 0;
	}
}

/* TODO: maintain the block count as the blocks are created/deleted */
static int
count_blocks(jit_function_t func)
{
	int count;
	jit_block_t block;

	count = 0;
	for(block = func->builder->entry_block; block; block = block->next)
	{
		++count;
	}
	return count;
}

/* Release block order memory */
static void
free_order(jit_function_t func)
{
	jit_free(func->builder->block_order);
	func->builder->block_order = NULL;
	func->builder->num_block_order = 0;
}

int
_jit_block_init(jit_function_t func)
{
	func->builder->entry_block = _jit_block_create(func);
	if(!func->builder->entry_block)
	{
		return 0;
	}

	func->builder->exit_block = _jit_block_create(func);
	if(!func->builder->exit_block)
	{
		return 0;
	}

	func->builder->entry_block->next = func->builder->exit_block;
	func->builder->exit_block->prev = func->builder->entry_block;
	return 1;
}

void
_jit_block_free(jit_function_t func)
{
	jit_block_t block, next;

	free_order(func);

	block = func->builder->entry_block;
	while(block)
	{
		next = block->next;
		_jit_block_destroy(block);
		block = next;
	}

	block = func->builder->deleted_blocks;
	while(block)
	{
		next = block->next;
		_jit_block_destroy(block);
		block = next;
	}

	func->builder->entry_block = 0;
	func->builder->exit_block = 0;
}

void
_jit_block_build_cfg(jit_function_t func)
{
	/* Count the edges */
	build_edges(func, 0);

	/* Allocate memory for edges */
	alloc_edges(func);

	/* Actually build the edges */
	build_edges(func, 1);
}

void
_jit_block_clean_cfg(jit_function_t func)
{
	int index, changed;
	jit_block_t block;
	jit_insn_t insn;

	/*
	 * The code below is based on the Clean algorithm described in
	 * "Engineering a Compiler" by Keith D. Cooper and Linda Torczon,
	 * section 10.3.1 "Eliminating Useless and Unreachable Code"
	 * (originally presented in a paper by Rob Shillner and John Lu
	 * http://www.cs.princeton.edu/~ras/clean.ps).
	 *
	 * Because libjit IR differs from ILOC the algorithm here has
	 * some differences too.
	 */

	if(!_jit_block_compute_postorder(func))
	{
		jit_exception_builtin(JIT_RESULT_OUT_OF_MEMORY);
	}
	eliminate_unreachable(func);

 loop:
	changed = 0;

	/* Go through blocks in post order skipping the entry and exit blocks */
	for(index = 1; index < (func->builder->num_block_order - 1); index++)
	{
		block = func->builder->block_order[index];
		if(block->num_succs == 0)
		{
			continue;
		}
		if(block->succs[0]->flags == _JIT_EDGE_BRANCH)
		{
			if(block->succs[0]->dst == block->next)
			{
				/* Replace useless branch with NOP */
				changed = 1;
				insn = _jit_block_get_last(block);
				insn->opcode = JIT_OP_NOP;
				if(block->num_succs == 1)
				{
					/* For unconditional branch replace the branch
					   edge with a fallthrough edge */
					block->ends_in_dead = 0;
					block->succs[0]->flags = _JIT_EDGE_FALLTHRU;
				}
				else
				{
					/* For conditional branch delete the branch
					   edge while leaving the fallthough edge */
					delete_edge(func, block->succs[0]);
				}
			}
			else if(block->num_succs == 2 && block->next->num_succs == 1
				&& block->next->succs[0]->flags == _JIT_EDGE_BRANCH
				&& block->succs[0]->dst == block->next->succs[0]->dst
				&& is_empty_block(block->next))
			{
				/* Replace conditional branch with unconditional,
				   remove the fallthough edge while leaving the branch
				   edge */
				changed = 1;
				insn = _jit_block_get_last(block);
				insn->opcode = JIT_OP_BR;
				block->ends_in_dead = 1;
				delete_edge(func, block->succs[1]);
			}
		}
		if(block->num_succs == 1
		   && (block->succs[0]->flags == _JIT_EDGE_BRANCH
		       || block->succs[0]->flags == _JIT_EDGE_FALLTHRU))
		{
			if(is_empty_block(block))
			{
				/* Remove empty block */
				merge_empty(func, block, &changed);
			}
		}

		/* TODO: "combine blocks" and "hoist branch" parts of the Clean algorithm */
	}

	if(changed)
	{
		if(!_jit_block_compute_postorder(func))
		{
			jit_exception_builtin(JIT_RESULT_OUT_OF_MEMORY);
		}
		clear_visited(func);
		goto loop;
	}
}

int
_jit_block_compute_postorder(jit_function_t func)
{
	int num_blocks, index, num, top;
	jit_block_t *blocks, block, succ;
	_jit_block_stack_entry_t *stack;

	if(func->builder->block_order)
	{
		free_order(func);
	}

	num_blocks = count_blocks(func);

	blocks = (jit_block_t *) jit_malloc(num_blocks * sizeof(jit_block_t));
	if(!blocks)
	{
		return 0;
	}

	stack = (_jit_block_stack_entry_t *) jit_malloc(num_blocks * sizeof(_jit_block_stack_entry_t));
	if(!stack)
	{
		jit_free(blocks);
		return 0;
	}

	func->builder->entry_block->visited = 1;
	stack[0].block = func->builder->entry_block;
	stack[0].index = 0;
	top = 1;
	num = 0;
	do
	{
		block = stack[top - 1].block;
		index = stack[top - 1].index;

		if(index == block->num_succs)
		{
			blocks[num++] = block;
			--top;
		}
		else
		{
			succ = block->succs[index]->dst;
			if(succ->visited)
			{
				stack[top - 1].index = index + 1;
			}
			else
			{
				succ->visited = 1;
				stack[top].block = succ;
				stack[top].index = 0;
				++top;
			}
		}
	}
	while(top);

	jit_free(stack);
	if(num < num_blocks)
	{
		blocks = jit_realloc(blocks, num * sizeof(jit_block_t));
	}

	func->builder->block_order = blocks;
	func->builder->num_block_order = num;
	return 1;
}

jit_block_t
_jit_block_create(jit_function_t func)
{
	jit_block_t block;

	/* Allocate memory for the block */
	block = jit_cnew(struct _jit_block);
	if(!block)
	{
		return 0;
	}

	/* Initialize the block */
	block->func = func;
	block->label = jit_label_undefined;

	return block;
}

void
_jit_block_destroy(jit_block_t block)
{
	/* Free all the memory owned by the block. CFG edges are not freed
	   because each edge is shared between two blocks so the ownership
	   of the edge is ambiguous. Sometimes an edge may be redirected to
	   another block rather than freed. Therefore edges are freed (or
	   not freed) separately. However succs and preds arrays are freed,
	   these contain pointers to edges, not edges themselves. */
	jit_meta_destroy(&block->meta);
	jit_free(block->succs);
	jit_free(block->preds);
	jit_free(block->insns);
	jit_free(block);
}

void
_jit_block_detach(jit_block_t first, jit_block_t last)
{
	last->next->prev = first->prev;
	first->prev->next = last->next;
}

void
_jit_block_attach_after(jit_block_t block, jit_block_t first, jit_block_t last)
{
	first->prev = block;
	last->next = block->next;
	block->next->prev = last;
	block->next = first;
}

void
_jit_block_attach_before(jit_block_t block, jit_block_t first, jit_block_t last)
{
	first->prev = block->prev;
	last->next = block;
	block->prev->next = first;
	block->prev = last;
}

int
_jit_block_record_label(jit_block_t block, jit_label_t label)
{
	jit_builder_t builder;
	jit_label_t num;
	_jit_label_info_t *info;

	builder = block->func->builder;
	if(label >= builder->max_label_info)
	{
		num = builder->max_label_info;
		if(num < 64)
		{
			num = 64;
		}
		while(num <= label)
		{
			num *= 2;
		}

		info = (_jit_label_info_t *) jit_realloc(builder->label_info,
							 num * sizeof(_jit_label_info_t));
		if(!info)
		{
			return 0;
		}

		jit_memzero(info + builder->max_label_info,
			    sizeof(_jit_label_info_t) * (num - builder->max_label_info));
		builder->label_info = info;
		builder->max_label_info = num;
	}

	builder->label_info[label].block = block;
	builder->label_info[label].alias = block->label;
	block->label = label;

	return 1;
}

jit_insn_t
_jit_block_add_insn(jit_block_t block)
{
	int max_insns;
	jit_insn_t insns;

	/* Make space for the instruction in the block's instruction list */
	if(block->num_insns == block->max_insns)
	{
		max_insns = block->max_insns ? block->max_insns * 2 : 4;
		insns = (jit_insn_t) jit_realloc(block->insns,
						 max_insns * sizeof(struct _jit_insn));
		if(!insns)
		{
			return 0;
		}

		block->insns = insns;
		block->max_insns = max_insns;
	}

	/* Zero-init the instruction */
	jit_memzero(&block->insns[block->num_insns], sizeof(struct _jit_insn));

	/* Return the instruction, which is now ready to fill in */
	return &block->insns[block->num_insns++];
}

jit_insn_t
_jit_block_get_last(jit_block_t block)
{
	if(block->num_insns > 0)
	{
		return &block->insns[block->num_insns - 1];
	}
	else
	{
		return 0;
	}
}

int
_jit_block_is_final(jit_block_t block)
{
	for(block = block->next; block; block = block->next)
	{
		if(block->num_insns)
		{
			return 0;
		}
	}
	return 1;
}

/*@
 * @deftypefun jit_function_t jit_block_get_function (jit_block_t @var{block})
 * Get the function that a particular @var{block} belongs to.
 * @end deftypefun
@*/
jit_function_t
jit_block_get_function(jit_block_t block)
{
	if(block)
	{
		return block->func;
	}
	else
	{
		return 0;
	}
}

/*@
 * @deftypefun jit_context_t jit_block_get_context (jit_block_t @var{block})
 * Get the context that a particular @var{block} belongs to.
 * @end deftypefun
@*/
jit_context_t
jit_block_get_context(jit_block_t block)
{
	if(block)
	{
		return block->func->context;
	}
	else
	{
		return 0;
	}
}

/*@
 * @deftypefun jit_label_t jit_block_get_label (jit_block_t @var{block})
 * Get the label associated with a block.
 * @end deftypefun
@*/
jit_label_t
jit_block_get_label(jit_block_t block)
{
	if(block)
	{
		return block->label;
	}
	return jit_label_undefined;
}

/*@
 * @deftypefun jit_label_t jit_block_get_next_label (jit_block_t @var{block, jit_label_t @var{label}})
 * Get the next label associated with a block.
 * @end deftypefun
@*/
jit_label_t
jit_block_get_next_label(jit_block_t block, jit_label_t label)
{
	jit_builder_t builder;
	if(block)
	{
		if(label == jit_label_undefined)
		{
			return block->label;
		}
		builder = block->func->builder;
		if(builder
		   && label < builder->max_label_info
		   && block == builder->label_info[label].block)
		{
			return builder->label_info[label].alias;
		}
	}
	return jit_label_undefined;
}

/*@
 * @deftypefun jit_block_t jit_block_next (jit_function_t @var{func}, jit_block_t @var{previous})
 * Iterate over the blocks in a function, in order of their creation.
 * The @var{previous} argument should be NULL on the first call.
 * This function will return NULL if there are no further blocks to iterate.
 * @end deftypefun
@*/
jit_block_t
jit_block_next(jit_function_t func, jit_block_t previous)
{
	if(previous)
	{
		return previous->next;
	}
	else if(func && func->builder)
	{
		return func->builder->entry_block;
	}
	else
	{
		return 0;
	}
}

/*@
 * @deftypefun jit_block_t jit_block_previous (jit_function_t @var{func}, jit_block_t @var{previous})
 * Iterate over the blocks in a function, in reverse order of their creation.
 * The @var{previous} argument should be NULL on the first call.
 * This function will return NULL if there are no further blocks to iterate.
 * @end deftypefun
@*/
jit_block_t
jit_block_previous(jit_function_t func, jit_block_t previous)
{
	if(previous)
	{
		return previous->prev;
	}
	else if(func && func->builder)
	{
		return func->builder->exit_block;
	}
	else
	{
		return 0;
	}
}

/*@
 * @deftypefun jit_block_t jit_block_from_label (jit_function_t @var{func}, jit_label_t @var{label})
 * Get the block that corresponds to a particular @var{label}.
 * Returns NULL if there is no block associated with the label.
 * @end deftypefun
@*/
jit_block_t
jit_block_from_label(jit_function_t func, jit_label_t label)
{
	if(func && func->builder && label < func->builder->max_label_info)
	{
		return func->builder->label_info[label].block;
	}
	else
	{
		return 0;
	}
}

/*@
 * @deftypefun int jit_block_set_meta (jit_block_t @var{block}, int @var{type}, void *@var{data}, jit_meta_free_func @var{free_data})
 * Tag a block with some metadata.  Returns zero if out of memory.
 * If the @var{type} already has some metadata associated with it, then
 * the previous value will be freed.  Metadata may be used to store
 * dependency graphs, branch prediction information, or any other
 * information that is useful to optimizers or code generators.
 *
 * Metadata type values of 10000 or greater are reserved for internal use.
 * @end deftypefun
@*/
int
jit_block_set_meta(jit_block_t block, int type, void *data, jit_meta_free_func free_data)
{
	return jit_meta_set(&(block->meta), type, data, free_data, block->func);
}

/*@
 * @deftypefun {void *} jit_block_get_meta (jit_block_t @var{block}, int @var{type})
 * Get the metadata associated with a particular tag.  Returns NULL
 * if @var{type} does not have any metadata associated with it.
 * @end deftypefun
@*/
void *
jit_block_get_meta(jit_block_t block, int type)
{
	return jit_meta_get(block->meta, type);
}

/*@
 * @deftypefun void jit_block_free_meta (jit_block_t @var{block}, int @var{type})
 * Free metadata of a specific type on a block.  Does nothing if
 * the @var{type} does not have any metadata associated with it.
 * @end deftypefun
@*/
void
jit_block_free_meta(jit_block_t block, int type)
{
	jit_meta_free(&(block->meta), type);
}

/*@
 * @deftypefun int jit_block_is_reachable (jit_block_t @var{block})
 * Determine if a block is reachable from some other point in
 * its function.  Unreachable blocks can be discarded in their
 * entirety.  If the JIT is uncertain as to whether a block is
 * reachable, or it does not wish to perform expensive flow
 * analysis to find out, then it will err on the side of caution
 * and assume that it is reachable.
 * @end deftypefun
@*/
int
jit_block_is_reachable(jit_block_t block)
{
	jit_block_t entry;

	/* Simple-minded reachability analysis that bothers only with
	   fall-through control flow. The block is considered reachable
	   if a) it is the entry block b) it has any label c) there is
	   fall-through path to it from one of the above. */
	entry = block->func->builder->entry_block;
	while(block != entry && block->label == jit_label_undefined)
	{
		block = block->prev;
		if(block->ends_in_dead)
		{
			/* There is no fall-through path from the prev block */
			return 0;
		}
	}

	return 1;
}

/*@
 * @deftypefun int jit_block_ends_in_dead (jit_block_t @var{block})
 * Determine if a block ends in a "dead" marker.  That is, control
 * will not fall out through the end of the block.
 * @end deftypefun
@*/
int
jit_block_ends_in_dead(jit_block_t block)
{
	return block->ends_in_dead;
}

/*@
 * @deftypefun int jit_block_current_is_dead (jit_function_t @var{func})
 * Determine if the current point in the function is dead.  That is,
 * there are no existing branches or fall-throughs to this point.
 * This differs slightly from @code{jit_block_ends_in_dead} in that
 * this can skip past zero-length blocks that may not appear to be
 * dead to find the dead block at the head of a chain of empty blocks.
 * @end deftypefun
@*/
int
jit_block_current_is_dead(jit_function_t func)
{
	jit_block_t block = jit_block_previous(func, 0);
	return !block || jit_block_ends_in_dead(block) || !jit_block_is_reachable(block);
}
