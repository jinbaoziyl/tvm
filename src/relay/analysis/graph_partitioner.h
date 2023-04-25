/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file src/relay/analysis/graph_partitioner.h
 * \brief The helper function for op fusion.
 */

#ifndef TVM_RELAY_ANALYSIS_GRAPH_PARTITIONER_H_
#define TVM_RELAY_ANALYSIS_GRAPH_PARTITIONER_H_

#include <tvm/relay/op_attr_types.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../support/arena.h"
#include "../transforms/let_list.h"

namespace tvm {
namespace relay {

using support::LinkedList;
using support::LinkNode;

/*!
 * \brief Indexed data flow graph in forward direction.
 *  This is a temporary data structure used for operator fusion analysis.
 *
 *  This data structure only captures the dataflow fragment and
 *  could ignore blocks like let by simply ordering each dataflow block
 *  and mark the output node as extern_ref;
 */
class IndexedForwardGraph {
 public:
  // base utils
  std::string get_pattern_kind(const OpPatternKind& kind) {
    std::string kind_name = "kOpaque";
    switch (kind) {
      case kElemWise:
        kind_name = "kElemWise";
        break;
      case kBroadcast:
        kind_name = "kBroadcast";
        break;
      case kInjective:
        kind_name = "kInjective";
        break;
      case kCommReduce:
        kind_name = "kCommReduce";
        break;
      case kOutEWiseFusable:
        kind_name = "kOutEWiseFusable";
        break;
      case kTuple:
        kind_name = "kTuple";
        break;
      default:
        break;
    }
    return kind_name;
  }

  // for IndexedForwardGraph, add to IndexedForwardGraph class
  void visualize(const std::string& file_path) {
    std::ofstream out(file_path, std::ofstream::binary);
    if (out.is_open()) {
      std::streambuf* coutbuf = std::cout.rdbuf();
      std::cout.rdbuf(out.rdbuf());
      // write the nodes
      std::cout << "name : \"dependency\"\n";
      for (auto it = this->post_dfs_order.rbegin(); it != this->post_dfs_order.rend(); ++it) {
        Node* n = *it;
        auto iit = n->outputs.head;
        std::cout << "layer {  name:\"Node_" << n->index << "\"\n";
        // add topo information
        std::cout << "  top : \"Node_" << n->index << "\"\n";
        if (iit != nullptr) {
          for (; iit != nullptr; iit = iit->next) {
            std::cout << "  bottom : \"Node_" << iit->value.node->index << "\"\n";
          }
        }
        // add type
        auto expr = GetRef<ObjectRef>(n->ref);
        auto pattern_name = get_pattern_kind(n->pattern);
        if (!expr.defined()) {
          std::cout << "  type : \"Connect[" << pattern_name << "]\"\n";
        } else if (expr.as<CallNode>()) {
          auto call = Downcast<Call>(expr);
          auto op = Downcast<Op>(call->op);
          std::cout << "  type : \"Call_" << op->name << "[" << pattern_name << "]\"\n";
        } else if (expr.as<ConstantNode>()) {
          std::cout << "  type : \"Constant[" << pattern_name << "]\"\n";
        } else if (expr.as<FunctionNode>()) {
          std::cout << "  type : \"Function[" << pattern_name << "]\"\n";
        } else if (expr.as<TupleGetItemNode>()) {
          auto node = Downcast<TupleGetItem>(expr);
          std::cout << "  type : \"TupleGetItemNode[" << pattern_name << "]\"\n";
        } else if (expr.as<OpNode>()) {
          auto node = Downcast<Op>(expr);
          std::cout << "  type : \"Op_" << node->name << "[" << pattern_name << "]\"\n";
        } else if (expr.as<VarNode>()) {
          auto node = Downcast<Var>(expr);
          std::cout << "  type : \"Var[" << pattern_name << "]\""
                    << "\n";
        } else {
          std::cout << "  type : \"UNKNOWN[" << pattern_name << "]\""
                    << "\n";
        }
        // add attributes
        std::cout << "  layer_param : {\n";
        std::cout << "    extern_ref : \"" << (n->extern_ref ? "true" : "false") << "\"\n";
        if (expr.as<TupleGetItemNode>()) {
          auto node = Downcast<TupleGetItem>(expr);
          std::cout << "    index : " << node->index << "\n";
        } else if (expr.as<ConstantNode>()) {
          auto node = Downcast<Constant>(expr);
          std::cout << "    tensor_type : \"" << node->tensor_type() << "\"\n";
        } else if (expr.as<VarNode>()) {
          auto node = Downcast<Var>(expr);
          std::cout << "    name_hint : \"" << node->name_hint() << "\"\n";
        }
        std::cout << "  }\n}\n";
      }
      std::cout.rdbuf(coutbuf);
      out.close();
    }
  }
  struct Node;
  /*!
   * The forward edge in the dataflow graph.
   */
  struct Edge {
    /*! \brief The corresponding node */
    Node* node{nullptr};
    /*! \brief The respective pattern of this op */
    OpPatternKind pattern{kOpaque};
  };
  /*! \brief A node in the graph. */
  struct Node {
    /*! \brief weak reference to the corresponding edge. */
    const tvm::Object* ref{nullptr};
    /*! \brief The index of the node in topological order. */
    size_t index{0};
    /*! \brief Whether this node is referenced by external source */
    bool extern_ref{false};
    /*! \brief The general pattern in the node */
    OpPatternKind pattern{kOpaque};
    /*! \brief The outputs of the node. */
    LinkedList<Edge> outputs;
  };
  /*! \brief The node map that maps node to graph */
  std::unordered_map<const tvm::Object*, Node*> node_map;
  /*! \brief All the nodes in post DFS order */
  std::vector<Node*> post_dfs_order;

  /*! \brief Dump the graph into string. */
  void DebugDump() {
    std::ostringstream os;
    for (size_t i = 0; i < post_dfs_order.size(); ++i) {
      Node* node = post_dfs_order[i];
      os << "node[" << i << "], " << GetRef<ObjectRef>(node->ref) << " outputs=[";
      for (auto* link = node->outputs.head; link != nullptr; link = link->next) {
        os << link->value.node->index << ", ";
      }
      os << "]\n";
    }
    LOG(INFO) << os.str();
  }
};

/*!
 * \brief Dominator tree that represent domination or
 *  post domination relation of the node.
 */
class DominatorTree {
 public:
  // for DominatorTree, add to DominatorTree class
  void visualize(const std::string& file_path) {
    std::ofstream out(file_path, std::ofstream::binary);
    if (out.is_open()) {
      std::streambuf* coutbuf = std::cout.rdbuf();
      std::cout.rdbuf(out.rdbuf());
      // write the nodes
      std::cout << "name : \"dependency\"\n";
      for (auto it = this->nodes.rbegin(); it != this->nodes.rend(); ++it) {
        Node* node = *it;
        IndexedForwardGraph::Node* gnode = node->gnode;
        std::cout << "layer {  name:\"Node_" << gnode->index << "\"\n";
        // add topo information
        std::cout << "  top : \"Node_" << gnode->index << "\"\n";
        if (node->parent != nullptr) {
          std::cout << "  bottom : \"Node_" << node->parent->gnode->index << "\"\n";
        }
        // add type
        auto expr = GetRef<ObjectRef>(gnode->ref);
        auto pattern_name = get_pattern_kind(node->pattern);
        if (!expr.defined()) {
          std::cout << "  type : \"Connect\n[" << pattern_name << "]\"\n";
        } else if (expr.as<CallNode>()) {
          auto call = Downcast<Call>(expr);
          auto op = Downcast<Op>(call->op);
          std::cout << "  type : \"Call_" << op->name << "[" << pattern_name << "]\"\n";
        } else if (expr.as<ConstantNode>()) {
          std::cout << "  type : \"Constant[" << pattern_name << "]\"\n";
        } else if (expr.as<FunctionNode>()) {
          std::cout << "  type : \"Function[" << pattern_name << "]\"\n";
        } else if (expr.as<TupleGetItemNode>()) {
          std::cout << "  type : \"TupleGetItemNode[" << pattern_name << "]\"\n";
        } else if (expr.as<OpNode>()) {
          auto e_node = Downcast<Op>(expr);
          std::cout << "  type : \"Op_" << e_node->name << "[" << pattern_name << "]\"\n";
        } else if (expr.as<VarNode>()) {
          auto e_node = Downcast<Var>(expr);
          std::cout << "  type : \"Var[" << pattern_name << "]\""
                    << "\n";
        } else {
          std::cout << "  type : \"UNKNOWN[" << pattern_name << "]\""
                    << "\n";
        }
        // add attributes
        std::cout << "  layer_param : {\n";
        std::cout << "    depth : \"" << node->depth << "\"\n";
        if (expr.as<TupleGetItemNode>()) {
          auto e_node = Downcast<TupleGetItem>(expr);
          std::cout << "    index : " << e_node->index << "\n";
        } else if (expr.as<ConstantNode>()) {
          auto e_node = Downcast<Constant>(expr);
          std::cout << "    tensor_type : \"" << e_node->tensor_type() << "\"\n";
        } else if (expr.as<VarNode>()) {
          auto e_node = Downcast<Var>(expr);
          std::cout << "    name_hint : \"" << e_node->name_hint() << "\"\n";
        }
        std::cout << "  }\n}\n";
      }
      std::cout.rdbuf(coutbuf);
      out.close();
    }
  }

  /*!
   * \brief A node in the dominator tree.
   */
  struct Node {
    /*! \brief The node in the tree */
    IndexedForwardGraph::Node* gnode{nullptr};
    /*! \brief parent of the tree */
    Node* parent{nullptr};
    /*! \brief current depth*/
    int depth{0};
    /*! \brief aggregated pattern to parent */
    OpPatternKind pattern{kOpaque};
  };
  // index -> node.
  std::vector<Node*> nodes;
  /*!
   * \brief compute a post dominator relation for a given dataflow graph.
   * \param arena The arena used for node allocation.
   * \param graph The graph to be analyzed.
   * \return The dominator tree of the graph.
   * \note This algorithm makes use of the fact that graph is DAG,
   *       and runs a single pass algorithm via LCA (Least Common Ancestor)
   */
  static DominatorTree PostDom(support::Arena* arena, const IndexedForwardGraph& graph);

 private:
  // Combine pattern together.
  inline static OpPatternKind CombinePattern(OpPatternKind lhs, OpPatternKind rhs) {
    if (lhs > rhs) return lhs;
    return rhs;
  }
  /*!
   * \brief Find the least common ancestor of the two nodes.
   * \param lhs The left node.
   * \param rhs The right node.
   * \param edge_pattern
   *        The combined edge pattern across all the parents.
   * \return The least common ancestor of the two.
   */
  static Node* LeastCommonAncestor(Node* lhs, Node* rhs, OpPatternKind* edge_pattern);
  /*!
   * \brief Find the least common ancestor of a list of nodes.
   * \param nodes the nodes.
   * \param edge_pattern
   *        The combined edge pattern across all the parents.
   * \return The least common ancestor of all nodes.
   */
  Node* LeastCommonAncestor(const LinkedList<IndexedForwardGraph::Edge>& input_nodes,
                            OpPatternKind* edge_pattern);

  /*!
   * \brief Convert the Node from an IndexedForwardGraph Node into DomaintorTree Node.
   * \param arena The Arena.
   * \param gnode An IndexedForwardGraph Node.
   * \return The DominatorTree Node.
   */
  Node* GetNode(support::Arena* arena, IndexedForwardGraph::Node* gnode);
};

/*!
 * \brief A partition of the graph marked by union find data structure.
 */
class GraphPartitioner {
 public:
  explicit GraphPartitioner(support::Arena* arena, int opt_level, size_t max_fuse_depth)
      : arena_(arena), opt_level_(opt_level), max_fuse_depth_(max_fuse_depth) {}
  // for GraphPartitioner, add to GraphPartitioner class
  void visualize(const std::string& file_path) {
    std::unordered_map<Group*, std::string> group_names;
    std::unordered_map<const tvm::Object*, std::string> ref_names;
    std::ofstream out(file_path, std::ofstream::binary);
    if (out.is_open()) {
      std::streambuf* coutbuf = std::cout.rdbuf();
      std::cout.rdbuf(out.rdbuf());
      // build names map
      for (int i = 0; i < groups_.size(); i++) {
        Group* group = groups_[i];
        group_names[group] = "Node_" + std::to_string(i);
        if (group->root_ref != nullptr) {
          ref_names[group->root_ref] = "Node_" + std::to_string(i);
        }
      }
      // write the nodes
      std::cout << "name : \"graph_paritioner\"\n";
      for (int i = 0; i < groups_.size(); i++) {
        Group* group = groups_[i];
        std::cout << "layer {  name:\"" << group_names[group] << "\"\n";
        // add topo information
        std::cout << "  top : \"" << group_names[group] << "\"\n";
        if (group->parent != nullptr) {
          std::cout << "  bottom : \"" << group_names[group->parent] << "\"\n";
        }
        // add type
        auto expr = GetRef<ObjectRef>(group->root_ref);
        auto pattern_name = get_pattern_kind(group->pattern);
        if (!expr.defined()) {
          std::cout << "  type : \"Connect\n[" << pattern_name << "]\"\n";
        } else if (expr.as<CallNode>()) {
          auto call = Downcast<Call>(expr);
          auto op = Downcast<Op>(call->op);
          std::cout << "  type : \"Call_" << op->name << "[" << pattern_name << "]\"\n";
        } else if (expr.as<ConstantNode>()) {
          std::cout << "  type : \"Constant[" << pattern_name << "]\"\n";
        } else if (expr.as<FunctionNode>()) {
          std::cout << "  type : \"Function[" << pattern_name << "]\"\n";
        } else if (expr.as<TupleGetItemNode>()) {
          std::cout << "  type : \"TupleGetItemNode[" << pattern_name << "]\"\n";
        } else if (expr.as<OpNode>()) {
          auto e_node = Downcast<Op>(expr);
          std::cout << "  type : \"Op_" << e_node->name << "[" << pattern_name << "]\"\n";
        } else if (expr.as<VarNode>()) {
          auto e_node = Downcast<Var>(expr);
          std::cout << "  type : \"Var[" << pattern_name << "]\""
                    << "\n";
        } else {
          std::cout << "  type : \"UNKNOWN[" << pattern_name << "]\""
                    << "\n";
        }
        // add attributes
        std::cout << "  layer_param : {\n";
        if (group->anchor_ref != nullptr) {
          std::cout << "    anchor_ref : \"" << ref_names[group->anchor_ref] << "\"\n";
        }
        if (expr.as<TupleGetItemNode>()) {
          auto e_node = Downcast<TupleGetItem>(expr);
          std::cout << "    index : " << e_node->index << "\n";
        } else if (expr.as<ConstantNode>()) {
          auto e_node = Downcast<Constant>(expr);
          std::cout << "    tensor_type : \"" << e_node->tensor_type() << "\"\n";
        } else if (expr.as<VarNode>()) {
          auto e_node = Downcast<Var>(expr);
          std::cout << "    name_hint : \"" << e_node->name_hint() << "\"\n";
        }
        std::cout << "  }\n}\n";
      }
      std::cout.rdbuf(coutbuf);
      out.close();
    }
  }
  /*!
   * \brief Group as a union find data structure.
   */
  struct Group {
    /*! \brief The parent in the union find data structure. */
    Group* parent{nullptr};
    /*! \brief The pattern of the group */
    OpPatternKind pattern;
    /*! \brief reference to the root node. */
    const tvm::Object* root_ref{nullptr};
    /*!
     * \brief Reference to the anchor node,
     * this field is not nullptr only if pattern is kOutEWiseFusable.
     */
    const tvm::Object* anchor_ref{nullptr};
    /*!
     * \brief The number of nodes belonging to this group
     */
    uint32_t num_nodes{1};

    /*! \brief Optional attributes to annotate the grouped function. */
    runtime::Map<runtime::String, ObjectRef> attrs;
    /*!
     * \brief Find the group root, perform path compression
     * \return The root type node.
     */
    Group* FindRoot();
  };
  /*!
   * \brief Partition a graph.
   * \return group assignments of each node.
   */
  std::vector<Group*> Partition(const IndexedForwardGraph& graph);

 private:
  /*! \brief The internal arena for temporary space. */
  support::Arena* arena_;
  /*! \brief optimization level for fuse operation. */
  int opt_level_;
  /*! \brief The maximum number of operations in one fused function */
  size_t max_fuse_depth_;
  /*! \brief The internal groups. */
  std::vector<Group*> groups_;
  /*! \brief internal field used for deduplication */
  std::unordered_set<IndexedForwardGraph::Node*> visited_;
  // Internal implementation of CheckPath
  template <typename F>
  bool CheckPath_(IndexedForwardGraph::Node* src, IndexedForwardGraph::Node* sink, F fcond);

  /*!
   * \brief Check all the node and edge pattern
   *  between src and sink satisfies fcond.
   *
   * src is not checked.
   *
   * \param src The source node.
   * \param sink The termination node.
   * \param fcond The condition to be checked.
   * \tparam F the condition function, with signature
   * \note sink must be a post-dominator of src.
   */
  template <typename F>
  bool CheckPath(IndexedForwardGraph::Node* src, IndexedForwardGraph::Node* sink, F fcond);

  /*!
   * \brief Merge the child group to the parent.
   * \param child The child group.
   * \param parent The parent group.
   */
  void MergeFromTo(Group* child, Group* parent);

  // Internal implementation of CommitFuse
  void CommitFuse_(IndexedForwardGraph::Node* src, IndexedForwardGraph::Node* sink, Group* target);

  /*!
   * \brief Commit fusion operation.
   * \param src The source node.
   * \param sink The termination node.
   * \note sink must be a post-dominator of src.
   */
  void CommitFuse(IndexedForwardGraph::Node* src, IndexedForwardGraph::Node* sink);

  size_t CountNodesUptoSink_(IndexedForwardGraph::Node* src, IndexedForwardGraph::Node* sink);

  // Count the number of nodes in a fused subgraph if child is additionally fused.
  // dom_parent is already known to be a part of the subgraph.
  // For a diamond structure, there can be multiple paths connecting child and dom_parent.
  // All intermediate nodes between child and dom_parent are taken into account.
  // Since dom_parent can itself be an intermediate node in the subgraph, calling FindRoot()
  // is important for correct calculation.
  size_t CountFusedNodesWithNewChild(IndexedForwardGraph::Node* child,
                                     IndexedForwardGraph::Node* dom_parent);

  // Initialize the groups.
  void InitGroups(const IndexedForwardGraph& graph);

  // execute the fusion algorithm.
  void RunFuse(const IndexedForwardGraph& graph, const DominatorTree& post_dom_tree, int phase);
};

}  // namespace relay
}  // namespace tvm
#endif  // TVM_RELAY_ANALYSIS_GRAPH_PARTITIONER_H_
