"""Control-flow graph construction for parsed function bodies."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Node:
    id: int
    stmt: tuple          # statement tuple, or ('eval', expr, line)
    succs: list = field(default_factory=list)  # (target_id, polarity, cond)
    # polarity: 'true' | 'false' | None


class CFGBuilder:
    def __init__(self, func):
        self.func = func
        self.nodes = []
        self.pending_gotos = []   # (node_id, label)
        self.labels = {}          # name -> node_id
        self.case_nodes = []      # (node_id, expr_or_None) for innermost
        # switch stack handled via ctx list
        self.switch_stack = []
        self.loop_stack = []      # (break_cont, continue_target)
        self.break_stack = []     # nearest breakable continuation (loop/switch)
        self.loop_heads = set()   # node ids that are loop headers
        self.partial = list(func.partial)

    def new_node(self, stmt):
        n = Node(len(self.nodes), stmt)
        self.nodes.append(n)
        return n.id

    # emit(stmt, cont) -> entry node id (or cont if stmt is empty)
    def emit(self, stmt, cont):
        tag = stmt[0]
        if tag == "block":
            return self.emit_seq(stmt[1], cont)
        if tag == "empty":
            return cont
        if tag == "expr":
            nid = self.new_node(("eval", stmt[1], stmt[2]))
            self.nodes[nid].succs = [(cont, None, None)]
            return nid
        if tag == "decl":
            nid = self.new_node(stmt)
            self.nodes[nid].succs = [(cont, None, None)]
            return nid
        if tag == "return":
            nid = self.new_node(stmt)
            self.nodes[nid].succs = []
            return nid
        if tag == "break":
            if not self.break_stack:
                self.partial.append("break outside loop")
                nid = self.new_node(("opaque", stmt[1]))
                self.nodes[nid].succs = [(cont, None, None)]
                return nid
            nid = self.new_node(stmt)
            self.nodes[nid].succs = [(self.break_stack[-1], None, None)]
            return nid
        if tag == "continue":
            if not self.loop_stack:
                self.partial.append("continue outside loop")
                nid = self.new_node(("opaque", stmt[1]))
                self.nodes[nid].succs = [(cont, None, None)]
                return nid
            nid = self.new_node(stmt)
            self.nodes[nid].succs = [(self.loop_stack[-1][1], None, None)]
            return nid
        if tag == "goto":
            nid = self.new_node(stmt)
            self.pending_gotos.append((nid, stmt[1]))
            return nid
        if tag == "label":
            nid = self.new_node(stmt)
            if stmt[1] in self.labels:
                self.partial.append(f"duplicate label {stmt[1]}")
            else:
                self.labels[stmt[1]] = nid
            self.nodes[nid].succs = [(cont, None, None)]
            return nid
        if tag == "case":
            nid = self.new_node(stmt)
            if self.switch_stack:
                self.switch_stack[-1].append((nid, stmt[1]))
            else:
                self.partial.append("case outside switch")
            self.nodes[nid].succs = [(cont, None, None)]
            return nid
        if tag == "default":
            nid = self.new_node(stmt)
            if self.switch_stack:
                self.switch_stack[-1].append((nid, None))
            else:
                self.partial.append("default outside switch")
            self.nodes[nid].succs = [(cont, None, None)]
            return nid
        if tag == "opaque":
            nid = self.new_node(stmt)
            self.nodes[nid].succs = [(cont, None, None)]
            return nid
        if tag == "if":
            eval_id = self.new_node(("eval", stmt[1], stmt[4]))
            then_entry = self.emit(stmt[2], cont)
            else_entry = self.emit(stmt[3], cont) if stmt[3] is not None else cont
            self.nodes[eval_id].succs = [
                (then_entry, "true", stmt[1]),
                (else_entry, "false", stmt[1]),
            ]
            return eval_id
        if tag == "while":
            eval_id = self.new_node(("eval", stmt[1], stmt[3]))
            self.loop_heads.add(eval_id)
            self.loop_stack.append((cont, eval_id))
            self.break_stack.append(cont)
            body_entry = self.emit(stmt[2], eval_id)
            self.break_stack.pop()
            self.loop_stack.pop()
            self.nodes[eval_id].succs = [
                (body_entry, "true", stmt[1]),
                (cont, "false", stmt[1]),
            ]
            return eval_id
        if tag == "do":
            eval_id = self.new_node(("eval", stmt[2], stmt[3]))
            self.loop_heads.add(eval_id)
            self.loop_stack.append((cont, eval_id))
            self.break_stack.append(cont)
            body_entry = self.emit(stmt[1], eval_id)
            self.break_stack.pop()
            self.loop_stack.pop()
            self.nodes[eval_id].succs = [
                (body_entry, "true", stmt[2]),
                (cont, "false", stmt[2]),
            ]
            return body_entry
        if tag == "for":
            # ('for', init_decls, init_exprs, cond, incr, body, line)
            if stmt[3] is not None:
                cond_node = self.new_node(("eval", stmt[3], stmt[6]))
            else:
                cond_node = self.new_node(("eval", ("num", "1", stmt[6]),
                                           stmt[6]))
            self.loop_heads.add(cond_node)
            incr_entry = cond_node
            if stmt[4]:
                seq = [("expr", e, stmt[6]) for e in stmt[4]]
                incr_entry = self.emit_seq(seq, cond_node)
            self.loop_stack.append((cont, incr_entry))
            self.break_stack.append(cont)
            body_entry = self.emit(stmt[5], incr_entry)
            self.break_stack.pop()
            self.loop_stack.pop()
            if stmt[3] is not None:
                self.nodes[cond_node].succs = [
                    (body_entry, "true", stmt[3]),
                    (cont, "false", stmt[3]),
                ]
            else:
                self.nodes[cond_node].succs = [(body_entry, None, None)]
            init_stmts = []
            if stmt[1] is not None:
                init_stmts.append(("decl", stmt[1], stmt[6]))
            if stmt[2]:
                for e in stmt[2]:
                    init_stmts.append(("expr", e, stmt[6]))
            if init_stmts:
                return self.emit_seq(init_stmts, cond_node)
            return cond_node
        if tag == "switch":
            cases = []
            self.switch_stack.append(cases)
            self.break_stack.append(cont)
            body_entry = self.emit(stmt[2], cont)
            self.break_stack.pop()
            self.switch_stack.pop()
            eval_id = self.new_node(("eval", stmt[1], stmt[3]))
            has_default = any(e is None for _, e in cases)
            succs = [(nid, None, None) for nid, _ in cases]
            if not has_default:
                succs.append((cont, None, None))
            self.nodes[eval_id].succs = succs
            # body_entry may be unreachable if no case at top; that's fine
            return eval_id
        # unknown stmt kind
        self.partial.append(f"unsupported statement {tag}")
        nid = self.new_node(("opaque", stmt[-1] if stmt else 0))
        self.nodes[nid].succs = [(cont, None, None)]
        return nid

    def emit_seq(self, stmts, cont):
        for s in reversed(stmts):
            cont = self.emit(s, cont)
        return cont

    def build(self, body, exit_id):
        entry = self.emit(body, exit_id)
        # patch gotos
        for nid, label in self.pending_gotos:
            if label in self.labels:
                self.nodes[nid].succs = [(self.labels[label], None, None)]
            else:
                self.partial.append(f"goto to undefined label {label}")
                self.nodes[nid].succs = [(exit_id, None, None)]
        return entry, self.nodes
