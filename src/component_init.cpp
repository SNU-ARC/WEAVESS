//
// Created by MurphySL on 2020/10/23.
//

#include "weavess/component.h"

namespace weavess {

    // NN-Descent
    void ComponentInitNNDescent::InitInner() {

        // L ITER S R
        SetConfigs();

        // 添加随机点作为近邻
        init();

        NNDescent();

        // graph_ -> final_graph
        index->getFinalGraph().resize(index->getBaseLen());
        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            std::vector<Index::SimpleNeighbor> tmp;

            std::sort(index->graph_[i].pool.begin(), index->graph_[i].pool.end());

            for (auto &j : index->graph_[i].pool)
                tmp.push_back(Index::SimpleNeighbor(j.id, j.distance));

            index->getFinalGraph()[i] = tmp;

            // 内存释放
            std::vector<Index::Neighbor>().swap(index->graph_[i].pool);
            std::vector<unsigned>().swap(index->graph_[i].nn_new);
            std::vector<unsigned>().swap(index->graph_[i].nn_old);
            std::vector<unsigned>().swap(index->graph_[i].rnn_new);
            std::vector<unsigned>().swap(index->graph_[i].rnn_new);
        }

        // 内存释放
        std::vector<Index::nhood>().swap(index->graph_);
    }

    void ComponentInitNNDescent::SetConfigs() {
        index->L = index->getParam().get<unsigned>("L");
        index->S = index->getParam().get<unsigned>("S");
        index->R = index->getParam().get<unsigned>("R");
        index->ITER = index->getParam().get<unsigned>("ITER");
    }

    void ComponentInitNNDescent::init() {
        index->graph_.reserve(index->getBaseLen());
        std::mt19937 rng(rand());
        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            index->graph_.emplace_back(Index::nhood(index->L, index->S, rng, (unsigned) index->getBaseLen()));
        }
#ifdef PARALLEL
#pragma omp parallel for
#endif
        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            std::vector<unsigned> tmp(index->S + 1);

            weavess::GenRandom(rng, tmp.data(), index->S + 1, index->getBaseLen());

            for (unsigned j = 0; j < index->S; j++) {
                unsigned id = tmp[j];

                if (id == i)continue;
                float dist = index->getDist()->compare(index->getBaseData() + i * index->getBaseDim(),
                                                       index->getBaseData() + id * index->getBaseDim(),
                                                       (unsigned) index->getBaseDim());

                index->graph_[i].pool.emplace_back(Index::Neighbor(id, dist, true));
            }
            std::make_heap(index->graph_[i].pool.begin(), index->graph_[i].pool.end());
            index->graph_[i].pool.reserve(index->L);
        }
    }

    void ComponentInitNNDescent::NNDescent() {
        for (unsigned it = 0; it < index->ITER; it++) {
            std::cout << "NN-Descent iter: " << it << std::endl;

            join();

            update();
        }
    }

    void ComponentInitNNDescent::join() {
#ifdef PARALLEL
#pragma omp parallel for default(shared) schedule(dynamic, 100)
#endif
        for (unsigned n = 0; n < index->getBaseLen(); n++) {
            index->graph_[n].join([&](unsigned i, unsigned j) {
                if (i != j) {
                    float dist = index->getDist()->compare(index->getBaseData() + i * index->getBaseDim(),
                                                           index->getBaseData() + j * index->getBaseDim(),
                                                           index->getBaseDim());

                    index->graph_[i].insert(j, dist);
                    index->graph_[j].insert(i, dist);
                }
            });
        }
    }

    void ComponentInitNNDescent::update() {
#ifdef PARALLEL
#pragma omp parallel for
#endif
        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            std::vector<unsigned>().swap(index->graph_[i].nn_new);
            std::vector<unsigned>().swap(index->graph_[i].nn_old);
            //std::vector<unsigned>().swap(graph_[i].rnn_new);
            //std::vector<unsigned>().swap(graph_[i].rnn_old);
            //graph_[i].nn_new.clear();
            //graph_[i].nn_old.clear();
            //graph_[i].rnn_new.clear();
            //graph_[i].rnn_old.clear();
        }
#ifdef PARALLEL
#pragma omp parallel for
#endif
        for (unsigned n = 0; n < index->getBaseLen(); ++n) {
            auto &nn = index->graph_[n];
            std::sort(nn.pool.begin(), nn.pool.end());
            if (nn.pool.size() > index->L)nn.pool.resize(index->L);
            nn.pool.reserve(index->L);
            unsigned maxl = std::min(nn.M + index->S, (unsigned) nn.pool.size());
            unsigned c = 0;
            unsigned l = 0;
            //std::sort(nn.pool.begin(), nn.pool.end());
            //if(n==0)std::cout << nn.pool[0].distance<<","<< nn.pool[1].distance<<","<< nn.pool[2].distance<< std::endl;
            while ((l < maxl) && (c < index->S)) {
                if (nn.pool[l].flag) ++c;
                ++l;
            }
            nn.M = l;
        }
#ifdef PARALLEL
#pragma omp parallel for
#endif
        for (unsigned n = 0; n < index->getBaseLen(); ++n) {
            auto &nnhd = index->graph_[n];
            auto &nn_new = nnhd.nn_new;
            auto &nn_old = nnhd.nn_old;
            for (unsigned l = 0; l < nnhd.M; ++l) {
                auto &nn = nnhd.pool[l];
                auto &nhood_o = index->graph_[nn.id];  // nn on the other side of the edge

                if (nn.flag) {
                    nn_new.push_back(nn.id);
                    if (nn.distance > nhood_o.pool.back().distance) {
                        Index::LockGuard guard(nhood_o.lock);
                        if (nhood_o.rnn_new.size() < index->R)nhood_o.rnn_new.push_back(n);
                        else {
                            unsigned int pos = rand() % index->R;
                            nhood_o.rnn_new[pos] = n;
                        }
                    }
                    nn.flag = false;
                } else {
                    nn_old.push_back(nn.id);
                    if (nn.distance > nhood_o.pool.back().distance) {
                        Index::LockGuard guard(nhood_o.lock);
                        if (nhood_o.rnn_old.size() < index->R)nhood_o.rnn_old.push_back(n);
                        else {
                            unsigned int pos = rand() % index->R;
                            nhood_o.rnn_old[pos] = n;
                        }
                    }
                }
            }
            std::make_heap(nnhd.pool.begin(), nnhd.pool.end());
        }
#ifdef PARALLEL
#pragma omp parallel for
#endif
        for (unsigned i = 0; i < index->getBaseLen(); ++i) {
            auto &nn_new = index->graph_[i].nn_new;
            auto &nn_old = index->graph_[i].nn_old;
            auto &rnn_new = index->graph_[i].rnn_new;
            auto &rnn_old = index->graph_[i].rnn_old;
            if (index->R && rnn_new.size() > index->R) {
                std::random_shuffle(rnn_new.begin(), rnn_new.end());
                rnn_new.resize(index->R);
            }
            nn_new.insert(nn_new.end(), rnn_new.begin(), rnn_new.end());
            if (index->R && rnn_old.size() > index->R) {
                std::random_shuffle(rnn_old.begin(), rnn_old.end());
                rnn_old.resize(index->R);
            }
            nn_old.insert(nn_old.end(), rnn_old.begin(), rnn_old.end());
            if (nn_old.size() > index->R * 2) {
                nn_old.resize(index->R * 2);
                nn_old.reserve(index->R * 2);
            }
            std::vector<unsigned>().swap(index->graph_[i].rnn_new);
            std::vector<unsigned>().swap(index->graph_[i].rnn_old);
        }
    }


    // RAND
    void ComponentInitRand::InitInner() {
        SetConfigs();

        index->graph_.resize(index->getBaseLen());
        std::mt19937 rng(rand());

#pragma omp parallel for
        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            std::vector<unsigned> tmp(index->L);

            weavess::GenRandom(rng, tmp.data(), index->L, index->getBaseLen());

            for (unsigned j = 0; j < index->L; j++) {
                unsigned id = tmp[j];

                if (id == i)continue;
                float dist = index->getDist()->compare(index->getBaseData() + i * index->getBaseDim(),
                                                       index->getBaseData() + id * index->getBaseDim(),
                                                       (unsigned) index->getBaseDim());

                index->graph_[i].pool.emplace_back(id, dist, true);
            }
            std::make_heap(index->graph_[i].pool.begin(), index->graph_[i].pool.end());
            index->graph_[i].pool.reserve(index->L);
        }

        index->getFinalGraph().resize(index->getBaseLen());
        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            std::vector<Index::SimpleNeighbor> tmp;

            std::sort(index->graph_[i].pool.begin(), index->graph_[i].pool.end());

            for (auto &j : index->graph_[i].pool) {
                tmp.push_back(Index::SimpleNeighbor(j.id, j.distance));
            }

            index->getFinalGraph()[i] = tmp;

            // 内存释放
            std::vector<Index::Neighbor>().swap(index->graph_[i].pool);
            std::vector<unsigned>().swap(index->graph_[i].nn_new);
            std::vector<unsigned>().swap(index->graph_[i].nn_old);
            std::vector<unsigned>().swap(index->graph_[i].rnn_new);
            std::vector<unsigned>().swap(index->graph_[i].rnn_new);
        }

        std::vector<Index::nhood>().swap(index->graph_);
    }

    void ComponentInitRand::SetConfigs() {
        index->L = index->getParam().get<unsigned>("L");
    }


    // KDT
    void ComponentInitKDT::InitInner() {
        SetConfigs();

        unsigned seed = 1998;

        index->graph_.resize(index->getBaseLen());
        index->knn_graph.resize(index->getBaseLen());

        const auto TreeNum = index->getParam().get<unsigned>("nTrees");
        const auto TreeNumBuild = index->getParam().get<unsigned>("nTrees");
        const auto K = index->getParam().get<unsigned>("K");

        // 选择树根
        std::vector<int> indices(index->getBaseLen());
        index->LeafLists.resize(TreeNum);
        std::vector<Index::EFANNA::Node *> ActiveSet;
        std::vector<Index::EFANNA::Node *> NewSet;
        for (unsigned i = 0; i < (unsigned) TreeNum; i++) {
            auto *node = new Index::EFANNA::Node;
            node->DivDim = -1;
            node->Lchild = nullptr;
            node->Rchild = nullptr;
            node->StartIdx = 0;
            node->EndIdx = index->getBaseLen();
            node->treeid = i;
            index->tree_roots_.push_back(node);
            ActiveSet.push_back(node);
        }

#pragma omp parallel for
        for (unsigned i = 0; i < index->getBaseLen(); i++)indices[i] = i;
#pragma omp parallel for
        for (unsigned i = 0; i < (unsigned) TreeNum; i++) {
            std::vector<unsigned> &myids = index->LeafLists[i];
            myids.resize(index->getBaseLen());
            std::copy(indices.begin(), indices.end(), myids.begin());
            std::random_shuffle(myids.begin(), myids.end());
        }
        omp_init_lock(&index->rootlock);
        // 构建随机截断树
        while (!ActiveSet.empty() && ActiveSet.size() < 1100) {
#pragma omp parallel for
            for (unsigned i = 0; i < ActiveSet.size(); i++) {
                Index::EFANNA::Node *node = ActiveSet[i];
                unsigned mid;
                unsigned cutdim;
                float cutval;
                std::mt19937 rng(seed ^ omp_get_thread_num());
                std::vector<unsigned> &myids = index->LeafLists[node->treeid];

                // 根据特征值进行划分
                meanSplit(rng, &myids[0] + node->StartIdx, node->EndIdx - node->StartIdx, mid, cutdim, cutval);

                node->DivDim = cutdim;
                node->DivVal = cutval;
                //node->StartIdx = offset;
                //node->EndIdx = offset + count;
                auto *nodeL = new Index::EFANNA::Node();
                auto *nodeR = new Index::EFANNA::Node();
                nodeR->treeid = nodeL->treeid = node->treeid;
                nodeL->StartIdx = node->StartIdx;
                nodeL->EndIdx = node->StartIdx + mid;
                nodeR->StartIdx = nodeL->EndIdx;
                nodeR->EndIdx = node->EndIdx;
                node->Lchild = nodeL;
                node->Rchild = nodeR;
                omp_set_lock(&index->rootlock);
                if (mid > K)NewSet.push_back(nodeL);
                if (nodeR->EndIdx - nodeR->StartIdx > K)NewSet.push_back(nodeR);
                omp_unset_lock(&index->rootlock);
            }
            ActiveSet.resize(NewSet.size());
            std::copy(NewSet.begin(), NewSet.end(), ActiveSet.begin());
            NewSet.clear();
        }

#pragma omp parallel for
        for (unsigned i = 0; i < ActiveSet.size(); i++) {
            Index::EFANNA::Node *node = ActiveSet[i];
            //omp_set_lock(&rootlock);
            //std::cout<<i<<":"<<node->EndIdx-node->StartIdx<<std::endl;
            //omp_unset_lock(&rootlock);
            std::mt19937 rng(seed ^ omp_get_thread_num());
            // 查找树根对应节点
            std::vector<unsigned> &myids = index->LeafLists[node->treeid];
            // 添加规定深度下的所有子节点
            DFSbuild(node, rng, &myids[0] + node->StartIdx, node->EndIdx - node->StartIdx, node->StartIdx);
        }
        //DFStest(0,0,tree_roots_[0]);
        std::cout << "build tree completed" << std::endl;

        for (size_t i = 0; i < (unsigned) TreeNumBuild; i++) {
            getMergeLevelNodeList(index->tree_roots_[i], i, 0);
        }

        std::cout << "merge node list size: " << index->mlNodeList.size() << std::endl;
        if (index->error_flag) {
            std::cout << "merge level deeper than tree, max merge deepth is " << index->max_deepth - 1 << std::endl;
        }

#pragma omp parallel for
        for (size_t i = 0; i < index->mlNodeList.size(); i++) {
            mergeSubGraphs(index->mlNodeList[i].second, index->mlNodeList[i].first);
        }

        std::cout << "merge tree completed" << std::endl;

        index->getFinalGraph().resize(index->getBaseLen());
        std::mt19937 rng(seed ^ omp_get_thread_num());
        std::set<Index::SimpleNeighbor> result;
        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            std::vector<std::vector<unsigned>> level_tmp;
            std::vector<Index::SimpleNeighbor> tmp;
            typename Index::CandidateHeap::reverse_iterator it = index->knn_graph[i].rbegin();
            for (; it != index->knn_graph[i].rend(); it++) {
                float dist = index->getDist()->compare(index->getBaseData() + index->getBaseDim() * i,
                                                       index->getBaseData() + index->getBaseDim() * it->row_id,
                                                       index->getBaseDim());
                tmp.push_back(Index::SimpleNeighbor(it->row_id, dist));
            }
            if (tmp.size() < K) {
                //std::cout << "node "<< i << " only has "<< tmp.size() <<" neighbors!" << std::endl;
                result.clear();
                size_t vlen = tmp.size();
                for (size_t j = 0; j < vlen; j++) {
                    result.insert(tmp[j]);
                }
                while (result.size() < K) {
                    unsigned id = rng() % index->getBaseLen();
                    float dist = index->getDist()->compare(index->getBaseData() + index->getBaseDim() * i,
                                                           index->getBaseData() + index->getBaseDim() * id,
                                                           index->getBaseDim());
                    result.insert(Index::SimpleNeighbor(id, dist));
                }
                tmp.clear();
                std::set<Index::SimpleNeighbor>::iterator it;
                for (it = result.begin(); it != result.end(); it++) {
                    tmp.push_back(*it);
                }
                //std::copy(result.begin(),result.end(),tmp.begin());
            }
            tmp.reserve(K);
            index->getFinalGraph()[i] = tmp;
        }
        std::vector<Index::nhood>().swap(index->graph_);
    }

    void ComponentInitKDT::SetConfigs() {
        index->mLevel = index->getParam().get<unsigned>("mLevel");
        index->nTrees = index->getParam().get<unsigned>("nTrees");
    }

    void ComponentInitKDT::meanSplit(std::mt19937 &rng, unsigned *indices, unsigned count, unsigned &index1,
                                     unsigned &cutdim, float &cutval) {
        float *mean_ = new float[index->getBaseDim()];
        float *var_ = new float[index->getBaseDim()];
        memset(mean_, 0, index->getBaseDim() * sizeof(float));
        memset(var_, 0, index->getBaseDim() * sizeof(float));

        /* Compute mean values.  Only the first SAMPLE_NUM values need to be
          sampled to get a good estimate.
         */
        unsigned cnt = std::min((unsigned) index->SAMPLE_NUM + 1, count);
        for (unsigned j = 0; j < cnt; ++j) {
            const float *v = index->getBaseData() + indices[j] * index->getBaseDim();
            for (size_t k = 0; k < index->getBaseDim(); ++k) {
                mean_[k] += v[k];
            }
        }
        float div_factor = float(1) / cnt;
        for (size_t k = 0; k < index->getBaseDim(); ++k) {
            mean_[k] *= div_factor;
        }

        /* Compute variances (no need to divide by count). */

        for (unsigned j = 0; j < cnt; ++j) {
            const float *v = index->getBaseData() + indices[j] * index->getBaseDim();
            for (size_t k = 0; k < index->getBaseDim(); ++k) {
                float dist = v[k] - mean_[k];
                var_[k] += dist * dist;
            }
        }

        /* Select one of the highest variance indices at random. */
        cutdim = selectDivision(rng, var_);

        cutval = mean_[cutdim];

        unsigned lim1, lim2;

        planeSplit(indices, count, cutdim, cutval, lim1, lim2);
        //cut the subtree using the id which best balances the tree
        if (lim1 > count / 2) index1 = lim1;
        else if (lim2 < count / 2) index1 = lim2;
        else index1 = count / 2;

        /* If either list is empty, it means that all remaining features
         * are identical. Split in the middle to maintain a balanced tree.
         */
        if ((lim1 == count) || (lim2 == 0)) index1 = count / 2;
        delete[] mean_;
        delete[] var_;
    }

    void ComponentInitKDT::planeSplit(unsigned *indices, unsigned count, unsigned cutdim, float cutval, unsigned &lim1,
                                      unsigned &lim2) {
        /* Move vector indices for left subtree to front of list. */
        int left = 0;
        int right = count - 1;
        for (;;) {
            const float *vl = index->getBaseData() + indices[left] * index->getBaseDim();
            const float *vr = index->getBaseData() + indices[right] * index->getBaseDim();
            while (left <= right && vl[cutdim] < cutval) {
                ++left;
                vl = index->getBaseData() + indices[left] * index->getBaseDim();
            }
            while (left <= right && vr[cutdim] >= cutval) {
                --right;
                vr = index->getBaseData() + indices[right] * index->getBaseDim();
            }
            if (left > right) break;
            std::swap(indices[left], indices[right]);
            ++left;
            --right;
        }
        lim1 = left;//lim1 is the id of the leftmost point <= cutval
        right = count - 1;
        for (;;) {
            const float *vl = index->getBaseData() + indices[left] * index->getBaseDim();
            const float *vr = index->getBaseData() + indices[right] * index->getBaseDim();
            while (left <= right && vl[cutdim] <= cutval) {
                ++left;
                vl = index->getBaseData() + indices[left] * index->getBaseDim();
            }
            while (left <= right && vr[cutdim] > cutval) {
                --right;
                vr = index->getBaseData() + indices[right] * index->getBaseDim();
            }
            if (left > right) break;
            std::swap(indices[left], indices[right]);
            ++left;
            --right;
        }
        lim2 = left;//lim2 is the id of the leftmost point >cutval
    }

    int ComponentInitKDT::selectDivision(std::mt19937 &rng, float *v) {
        int num = 0;
        size_t topind[index->RAND_DIM];

        //Create a list of the indices of the top index->RAND_DIM values.
        for (size_t i = 0; i < index->getBaseDim(); ++i) {
            if ((num < index->RAND_DIM) || (v[i] > v[topind[num - 1]])) {
                // Put this element at end of topind.
                if (num < index->RAND_DIM) {
                    topind[num++] = i;            // Add to list.
                } else {
                    topind[num - 1] = i;         // Replace last element.
                }
                // Bubble end value down to right location by repeated swapping. sort the varience in decrease order
                int j = num - 1;
                while (j > 0 && v[topind[j]] > v[topind[j - 1]]) {
                    std::swap(topind[j], topind[j - 1]);
                    --j;
                }
            }
        }
        // Select a random integer in range [0,num-1], and return that index.
        int rnd = rng() % num;
        return (int) topind[rnd];
    }

    void ComponentInitKDT::DFSbuild(Index::EFANNA::Node *node, std::mt19937 &rng, unsigned *indices, unsigned count,
                                    unsigned offset) {
        //omp_set_lock(&rootlock);
        //std::cout<<node->treeid<<":"<<offset<<":"<<count<<std::endl;
        //omp_unset_lock(&rootlock);

        if (count <= index->TNS) {
            node->DivDim = -1;
            node->Lchild = nullptr;
            node->Rchild = nullptr;
            node->StartIdx = offset;
            node->EndIdx = offset + count;
            //add points

        } else {
            unsigned idx;
            unsigned cutdim;
            float cutval;
            meanSplit(rng, indices, count, idx, cutdim, cutval);
            node->DivDim = cutdim;
            node->DivVal = cutval;
            node->StartIdx = offset;
            node->EndIdx = offset + count;
            auto *nodeL = new Index::EFANNA::Node();
            auto *nodeR = new Index::EFANNA::Node();
            node->Lchild = nodeL;
            nodeL->treeid = node->treeid;
            DFSbuild(nodeL, rng, indices, idx, offset);
            node->Rchild = nodeR;
            nodeR->treeid = node->treeid;
            DFSbuild(nodeR, rng, indices + idx, count - idx, offset + idx);
        }
    }

    void ComponentInitKDT::DFStest(unsigned level, unsigned dim, Index::EFANNA::Node *node) {
        if (node->Lchild != nullptr) {
            DFStest(++level, node->DivDim, node->Lchild);
            //if(level > 15)
            std::cout << "dim: " << node->DivDim << "--cutval: " << node->DivVal << "--S: " << node->StartIdx << "--E: "
                      << node->EndIdx << " TREE: " << node->treeid << std::endl;
            if (node->Lchild->Lchild == nullptr) {
                std::vector<unsigned> &tmp = index->LeafLists[node->treeid];
                for (unsigned i = node->Rchild->StartIdx; i < node->Rchild->EndIdx; i++) {
                    const float *tmpfea = index->getBaseData() + tmp[i] * index->getBaseDim() + node->DivDim;
                    std::cout << *tmpfea << " ";
                }
                std::cout << std::endl;
            }
        } else if (node->Rchild != nullptr) {
            DFStest(++level, node->DivDim, node->Rchild);
        } else {
            std::cout << "dim: " << dim << std::endl;
            std::vector<unsigned> &tmp = index->LeafLists[node->treeid];
            for (unsigned i = node->StartIdx; i < node->EndIdx; i++) {
                const float *tmpfea = index->getBaseData() + tmp[i] * index->getBaseDim() + dim;
                std::cout << *tmpfea << " ";
            }
            std::cout << std::endl;
        }
    }

    void ComponentInitKDT::getMergeLevelNodeList(Index::EFANNA::Node *node, size_t treeid, unsigned deepth) {
        auto ml = index->getParam().get<unsigned>("mLevel");
        if (node->Lchild != nullptr && node->Rchild != nullptr && deepth < ml) {
            deepth++;
            getMergeLevelNodeList(node->Lchild, treeid, deepth);
            getMergeLevelNodeList(node->Rchild, treeid, deepth);
        } else if (deepth == ml) {
            index->mlNodeList.emplace_back(node, treeid);
        } else {
            index->error_flag = true;
            if (deepth < index->max_deepth)index->max_deepth = deepth;
        }
    }

    Index::EFANNA::Node *ComponentInitKDT::SearchToLeaf(Index::EFANNA::Node *node, size_t id) {
        if (node->Lchild != nullptr && node->Rchild != nullptr) {
            const float *v = index->getBaseData() + id * index->getBaseDim();
            if (v[node->DivDim] < node->DivVal)
                return SearchToLeaf(node->Lchild, id);
            else
                return SearchToLeaf(node->Rchild, id);
        } else
            return node;
    }

    void ComponentInitKDT::mergeSubGraphs(size_t treeid, Index::EFANNA::Node *node) {
        auto K = index->getParam().get<unsigned>("K");

        if (node->Lchild != nullptr && node->Rchild != nullptr) {
            mergeSubGraphs(treeid, node->Lchild);
            mergeSubGraphs(treeid, node->Rchild);

            size_t numL = node->Lchild->EndIdx - node->Lchild->StartIdx;
            size_t numR = node->Rchild->EndIdx - node->Rchild->StartIdx;
            size_t start, end;
            Index::EFANNA::Node *root;
            if (numL < numR) {
                root = node->Rchild;
                start = node->Lchild->StartIdx;
                end = node->Lchild->EndIdx;
            } else {
                root = node->Lchild;
                start = node->Rchild->StartIdx;
                end = node->Rchild->EndIdx;
            }

            //std::cout << start << " " << end << std::endl;

            for (; start < end; start++) {

                size_t feature_id = index->LeafLists[treeid][start];

                Index::EFANNA::Node *leaf = SearchToLeaf(root, feature_id);
                for (size_t i = leaf->StartIdx; i < leaf->EndIdx; i++) {
                    size_t tmpfea = index->LeafLists[treeid][i];
                    float dist = index->getDist()->compare(index->getBaseData() + tmpfea * index->getBaseDim(),
                                                           index->getBaseData() + feature_id * index->getBaseDim(),
                                                           index->getBaseDim());

                    {
                        Index::LockGuard guard(index->graph_[tmpfea].lock);
                        if (index->knn_graph[tmpfea].size() < K || dist < index->knn_graph[tmpfea].begin()->distance) {
                            Index::Candidate c1(feature_id, dist);
                            index->knn_graph[tmpfea].insert(c1);
                            if (index->knn_graph[tmpfea].size() > K)
                                index->knn_graph[tmpfea].erase(index->knn_graph[tmpfea].begin());
                        }
                    }

                    {
                        Index::LockGuard guard(index->graph_[feature_id].lock);
                        if (index->knn_graph[feature_id].size() < K ||
                            dist < index->knn_graph[feature_id].begin()->distance) {
                            Index::Candidate c1(tmpfea, dist);
                            index->knn_graph[feature_id].insert(c1);
                            if (index->knn_graph[feature_id].size() > K)
                                index->knn_graph[feature_id].erase(index->knn_graph[feature_id].begin());

                        }
                    }
                }
            }
        }
    }


    // IEH
    void ComponentInitIEH::InitInner() {
        std::string func_argv = index->getParam().get<std::string>("func");
        std::string basecode_argv = index->getParam().get<std::string>("basecode");
        std::string knntable_argv = index->getParam().get<std::string>("knntable");

        Index::Matrix func;
        Index::Codes basecode;
        Index::Codes querycode;
        Index::Matrix train;
        Index::Matrix test;

        LoadHashFunc(&func_argv[0], func);
        LoadBaseCode(&basecode_argv[0], basecode);

        int UpperBits = 8;
        int LowerBits = 8; //change with code length:code length = up + low;
        Index::HashTable tb;
        BuildHashTable(UpperBits, LowerBits, basecode, tb);
        std::cout << "build hash table complete" << std::endl;

        QueryToCode(test, func, querycode);
        std::cout << "convert query code complete" << std::endl;
        std::vector<std::vector<int> > hashcands;
        HashTest(UpperBits, LowerBits, querycode, tb, hashcands);
        std::cout << "hash candidates ready" << std::endl;

        std::cout << "initial finish : " << std::endl;

        std::vector<Index::CandidateHeap2> knntable;
        LoadKnnTable(&knntable_argv[0], knntable);
        std::cout << "load knn graph complete" << std::endl;

        //GNNS
        std::vector<Index::CandidateHeap2> res;
        for (size_t i = 0; i < hashcands.size(); i++) {
            Index::CandidateHeap2 cands;
            for (size_t j = 0; j < hashcands[i].size(); j++) {
                int neighbor = hashcands[i][j];
                Index::Candidate2<float> c(neighbor,
                                           index->getDist()->compare(&test[i][0], &train[neighbor][0], test[i].size()));
                cands.insert(c);
                if (cands.size() > POOL_SIZE)cands.erase(cands.begin());
            }
            res.push_back(cands);
        }

        //iteration
        auto expand = index->getParam().get<unsigned>("expand");
        auto iterlimit = index->getParam().get<unsigned>("iterlimit");
        for (size_t i = 0; i < res.size(); i++) {
            int niter = 0;
            while (niter++ < iterlimit) {
                Index::CandidateHeap2::reverse_iterator it = res[i].rbegin();
                std::vector<int> ids;
                for (int j = 0; it != res[i].rend() && j < expand; it++, j++) {
                    int neighbor = it->row_id;
                    Index::CandidateHeap2::reverse_iterator nnit = knntable[neighbor].rbegin();
                    for (int k = 0; nnit != knntable[neighbor].rend() && k < expand; nnit++, k++) {
                        int nn = nnit->row_id;
                        ids.push_back(nn);
                    }
                }
                for (size_t j = 0; j < ids.size(); j++) {
                    Index::Candidate2<float> c(ids[j], index->getDist()->compare(&test[i][0], &train[ids[j]][0],
                                                                                 test[i].size()));
                    res[i].insert(c);
                    if (res[i].size() > POOL_SIZE)res[i].erase(res[i].begin());
                }
            }//cout<<i<<endl;
        }
        std::cout << "GNNS complete " << std::endl;
    }

    void StringSplit(std::string src, std::vector<std::string> &des) {
        int start = 0;
        int end = 0;
        for (size_t i = 0; i < src.length(); i++) {
            if (src[i] == ' ') {
                end = i;
                //if(end>start)cout<<start<<" "<<end<<" "<<src.substr(start,end-start)<<endl;
                des.push_back(src.substr(start, end - start));
                start = i + 1;
            }
        }
    }

    void ComponentInitIEH::LoadHashFunc(char *filename, Index::Matrix &func) {
        std::ifstream in(filename);
        char buf[MAX_ROWSIZE];

        while (!in.eof()) {
            in.getline(buf, MAX_ROWSIZE);
            std::string strtmp(buf);
            std::vector<std::string> strs;
            StringSplit(strtmp, strs);
            if (strs.size() < 2)continue;
            std::vector<float> ftmp;
            for (size_t i = 0; i < strs.size(); i++) {
                float f = atof(strs[i].c_str());
                ftmp.push_back(f);
                //cout<<f<<" ";
            }//cout<<endl;
            //cout<<strtmp<<endl;
            func.push_back(ftmp);
        }//cout<<func.size()<<endl;
        in.close();
    }

    void ComponentInitIEH::LoadBaseCode(char *filename, Index::Codes &base) {
        std::ifstream in(filename);
        char buf[MAX_ROWSIZE];
        //int cnt = 0;
        while (!in.eof()) {
            in.getline(buf, MAX_ROWSIZE);
            std::string strtmp(buf);
            std::vector<std::string> strs;
            StringSplit(strtmp, strs);
            if (strs.size() < 2)continue;
            unsigned int codetmp = 0;
            for (size_t i = 0; i < strs.size(); i++) {
                unsigned int c = atoi(strs[i].c_str());
                codetmp = codetmp << 1;
                codetmp += c;

            }//if(cnt++ > 999998){cout<<strs.size()<<" "<<buf<<" "<<codetmp<<endl;}
            base.push_back(codetmp);
        }//cout<<base.size()<<endl;
        in.close();
    }

    void ComponentInitIEH::BuildHashTable(int upbits, int lowbits, Index::Codes base, Index::HashTable &tb) {
        tb.clear();
        for (int i = 0; i < (1 << upbits); i++) {
            Index::HashBucket emptyBucket;
            tb.push_back(emptyBucket);
        }
        for (size_t i = 0; i < base.size(); i++) {
            unsigned int idx1 = base[i] >> lowbits;
            unsigned int idx2 = base[i] - (idx1 << lowbits);
            if (tb[idx1].find(idx2) != tb[idx1].end()) {
                tb[idx1][idx2].push_back(i);
            } else {
                std::vector<unsigned int> v;
                v.push_back(i);
                tb[idx1].insert(make_pair(idx2, v));
            }
        }
    }

    bool MatrixMultiply(Index::Matrix A, Index::Matrix B, Index::Matrix &C) {
        if (A.size() == 0 || B.size() == 0) {
            std::cout << "matrix a or b size 0" << std::endl;
            return false;
        } else if (A[0].size() != B.size()) {
            std::cout << "--error: matrix a, b dimension not agree" << std::endl;
            std::cout << "A" << A.size() << " * " << A[0].size() << std::endl;
            std::cout << "B" << B.size() << " * " << B[0].size() << std::endl;
            return false;
        }
        for (size_t i = 0; i < A.size(); i++) {
            std::vector<float> tmp;
            for (size_t j = 0; j < B[0].size(); j++) {
                float fnum = 0;
                for (size_t k = 0; k < B.size(); k++)fnum += A[i][k] * B[k][j];
                tmp.push_back(fnum);
            }
            C.push_back(tmp);
        }
        return true;
    }

    void ComponentInitIEH::QueryToCode(Index::Matrix query, Index::Matrix func, Index::Codes &querycode) {
        Index::Matrix Z;
        if (!MatrixMultiply(query, func, Z)) { return; }
        for (size_t i = 0; i < Z.size(); i++) {
            unsigned int codetmp = 0;
            for (size_t j = 0; j < Z[0].size(); j++) {
                if (Z[i][j] > 0) {
                    codetmp = codetmp << 1;
                    codetmp += 1;
                } else {
                    codetmp = codetmp << 1;
                    codetmp += 0;
                }
            }
            //if(i<3)cout<<codetmp<<endl;
            querycode.push_back(codetmp);
        }//cout<<querycode.size()<<endl;
    }

    void ComponentInitIEH::HashTest(int upbits, int lowbits, Index::Codes querycode, Index::HashTable tb,
                                    std::vector<std::vector<int> > &cands) {
        for (size_t i = 0; i < querycode.size(); i++) {

            unsigned int idx1 = querycode[i] >> lowbits;
            unsigned int idx2 = querycode[i] - (idx1 << lowbits);
            Index::HashBucket::iterator bucket = tb[idx1].find(idx2);
            std::vector<int> canstmp;
            if (bucket != tb[idx1].end()) {
                std::vector<unsigned int> vp = bucket->second;
                //cout<<i<<":"<<vp.size()<<endl;
                for (size_t j = 0; j < vp.size() && canstmp.size() < INIT_NUM; j++) {
                    canstmp.push_back(vp[j]);
                }
            }


            if (HASH_RADIUS == 0) {
                cands.push_back(canstmp);
                continue;
            }
            for (size_t j = 0; j < DEPTH; j++) {
                unsigned int searchcode = querycode[i] ^(1 << j);
                unsigned int idx1 = searchcode >> lowbits;
                unsigned int idx2 = searchcode - (idx1 << lowbits);
                Index::HashBucket::iterator bucket = tb[idx1].find(idx2);
                if (bucket != tb[idx1].end()) {
                    std::vector<unsigned int> vp = bucket->second;
                    for (size_t k = 0; k < vp.size() && canstmp.size() < INIT_NUM; k++) {
                        canstmp.push_back(vp[k]);
                    }
                }
            }
            cands.push_back(canstmp);
        }
    }

    void ComponentInitIEH::LoadKnnTable(char *filename, std::vector<Index::CandidateHeap2> &tb) {
        std::ifstream in(filename, std::ios::binary);
        in.seekg(0, std::ios::end);
        std::ios::pos_type ss = in.tellg();
        size_t fsize = (size_t) ss;
        int dim;
        in.seekg(0, std::ios::beg);
        in.read((char *) &dim, sizeof(int));
        size_t num = fsize / (dim + 1) / 4;
        std::cout << "load graph " << num << " " << dim << std::endl;
        in.seekg(0, std::ios::beg);
        tb.clear();
        for (size_t i = 0; i < num; i++) {
            Index::CandidateHeap2 heap;
            in.read((char *) &dim, sizeof(int));
            for (int j = 0; j < dim; j++) {
                int id;
                in.read((char *) &id, sizeof(int));
                Index::Candidate2<float> can(id, -1);
                heap.insert(can);
            }
            tb.push_back(heap);

        }
        in.close();
    }


    // NSW
    void ComponentInitNSW::InitInner() {
        SetConfigs();

        index->nodes_.resize(index->getBaseLen());
        Index::HnswNode *first = new Index::HnswNode(0, 0, index->NN_, index->NN_);
        index->nodes_[0] = first;
        index->enterpoint_ = first;
#pragma omp parallel num_threads(index->n_threads_)
        {
            auto *visited_list = new Index::VisitedList(index->getBaseLen());
#pragma omp for schedule(dynamic, 128)
            for (size_t i = 1; i < index->getBaseLen(); ++i) {
                auto *qnode = new Index::HnswNode(i, 0, index->NN_, index->NN_);
                index->nodes_[i] = qnode;
                InsertNode(qnode, visited_list);
            }
            delete visited_list;
        }
    }

    void ComponentInitNSW::SetConfigs() {
        index->NN_ = index->getParam().get<unsigned>("NN");
        index->ef_construction_ = index->getParam().get<unsigned>("ef_construction");
        index->n_threads_ = index->getParam().get<unsigned>("n_threads_");
    }

    void ComponentInitNSW::InsertNode(Index::HnswNode *qnode, Index::VisitedList *visited_list) {
        Index::HnswNode *enterpoint = index->enterpoint_;

        std::priority_queue<Index::FurtherFirst> result;
        std::priority_queue<Index::CloserFirst> tmp;

        // CANDIDATE
        SearchAtLayer(qnode, enterpoint, 0, visited_list, result);

        while (!result.empty()) {
            tmp.push(Index::CloserFirst(result.top().GetNode(), result.top().GetDistance()));
            result.pop();
        }

        int pos = 0;
        while (!tmp.empty() && pos < index->NN_) {
            auto *top_node = tmp.top().GetNode();
            tmp.pop();
            Link(top_node, qnode, 0);
            pos++;
        }
    }

    void ComponentInitNSW::SearchAtLayer(Index::HnswNode *qnode, Index::HnswNode *enterpoint, int level,
                                         Index::VisitedList *visited_list,
                                         std::priority_queue<Index::FurtherFirst> &result) {
        // TODO: check Node 12bytes => 8bytes
        std::priority_queue<Index::CloserFirst> candidates;
        float d = index->getDist()->compare(index->getBaseData() + qnode->GetId() * index->getBaseDim(),
                                            index->getBaseData() + enterpoint->GetId() * index->getBaseDim(),
                                            index->getBaseDim());
        result.emplace(enterpoint, d);
        candidates.emplace(enterpoint, d);

        visited_list->Reset();
        visited_list->MarkAsVisited(enterpoint->GetId());

        while (!candidates.empty()) {
            const Index::CloserFirst &candidate = candidates.top();
            float lower_bound = result.top().GetDistance();
            if (candidate.GetDistance() > lower_bound)
                break;

            Index::HnswNode *candidate_node = candidate.GetNode();
            std::unique_lock<std::mutex> lock(candidate_node->GetAccessGuard());
            const std::vector<Index::HnswNode *> &neighbors = candidate_node->GetFriends(level);
            candidates.pop();
            for (const auto &neighbor : neighbors) {
                int id = neighbor->GetId();
                if (visited_list->NotVisited(id)) {
                    visited_list->MarkAsVisited(id);
                    d = index->getDist()->compare(index->getBaseData() + qnode->GetId() * index->getBaseDim(),
                                                  index->getBaseData() + neighbor->GetId() * index->getBaseDim(),
                                                  index->getBaseDim());
                    if (result.size() < index->ef_construction_ || result.top().GetDistance() > d) {
                        result.emplace(neighbor, d);
                        candidates.emplace(neighbor, d);
                        if (result.size() > index->ef_construction_)
                            result.pop();
                    }
                }
            }
        }
    }

    void ComponentInitNSW::Link(Index::HnswNode *source, Index::HnswNode *target, int level) {
        source->AddFriends(target, true);
        target->AddFriends(source, true);
    }


    // HNSW
    void ComponentInitHNSW::InitInner() {
        SetConfigs();

        Build(false);
    }

    void ComponentInitHNSW::SetConfigs() {
        index->max_m_ = index->getParam().get<unsigned>("max_m");
        index->m_ = index->max_m_;
        index->max_m0_ = index->getParam().get<unsigned>("max_m0");
        index->ef_construction_ = index->getParam().get<unsigned>("ef_construction");
        index->n_threads_ = index->getParam().get<unsigned>("n_threads");
        index->mult = index->getParam().get<unsigned>("mult");
        index->level_mult_ = index->mult > 0 ? index->mult : 1 / log(1.0 * index->m_);
    }

    void ComponentInitHNSW::Build(bool reverse) {
        index->nodes_.resize(index->getBaseLen());

        int level = GetRandomNodeLevel();
        auto *first = new Index::HnswNode(0, level, index->max_m_, index->max_m0_);
        index->nodes_[0] = first;
        index->max_level_ = level;
        index->enterpoint_ = first;
#pragma omp parallel num_threads(index->n_threads_)
        {
            Index::VisitedList *visited_list = new Index::VisitedList(index->getBaseLen());
            if (reverse) {
#pragma omp for schedule(dynamic, 128)
                for (size_t i = index->getBaseLen() - 1; i >= 1; --i) {
                    int level = GetRandomNodeLevel();
                    Index::HnswNode *qnode = new Index::HnswNode(i, level, index->max_m_, index->max_m0_);
                    index->nodes_[i] = qnode;
                    InsertNode(qnode, visited_list);
                }
            } else {
#pragma omp for schedule(dynamic, 128)
                for (size_t i = 1; i < index->getBaseLen(); ++i) {
                    int level = GetRandomNodeLevel();
                    auto *qnode = new Index::HnswNode(i, level, index->max_m_, index->max_m0_);
                    index->nodes_[i] = qnode;
                    InsertNode(qnode, visited_list);
                }
            }
            delete visited_list;
        }
    }

    int ComponentInitHNSW::GetRandomSeedPerThread() {
        int tid = omp_get_thread_num();
        int g_seed = 17;
        for (int i = 0; i <= tid; ++i)
            g_seed = 214013 * g_seed + 2531011;
        return (g_seed >> 16) & 0x7FFF;
    }

    int ComponentInitHNSW::GetRandomNodeLevel() {
        static thread_local std::mt19937 rng(GetRandomSeedPerThread());
        static thread_local std::uniform_real_distribution<double> uniform_distribution(0.0, 1.0);
        double r = uniform_distribution(rng);

        if (r < std::numeric_limits<double>::epsilon())
            r = 1.0;
        return (int) (-log(r) * index->level_mult_);
    }

    void ComponentInitHNSW::InsertNode(Index::HnswNode *qnode, Index::VisitedList *visited_list) {
        int cur_level = qnode->GetLevel();
        std::unique_lock<std::mutex> max_level_lock(index->max_level_guard_, std::defer_lock);
        if (cur_level > index->max_level_)
            max_level_lock.lock();

        int max_level_copy = index->max_level_;
        Index::HnswNode *enterpoint = index->enterpoint_;

        if (cur_level < max_level_copy) {
            Index::HnswNode *cur_node = enterpoint;

            float d = index->getDist()->compare(index->getBaseData() + qnode->GetId() * index->getBaseDim(),
                                                index->getBaseData() + cur_node->GetId() * index->getBaseDim(),
                                                index->getBaseDim());
            float cur_dist = d;
            for (auto i = max_level_copy; i > cur_level; --i) {
                bool changed = true;
                while (changed) {
                    changed = false;
                    std::unique_lock<std::mutex> local_lock(cur_node->GetAccessGuard());
                    const std::vector<Index::HnswNode *> &neighbors = cur_node->GetFriends(i);

                    for (auto iter = neighbors.begin(); iter != neighbors.end(); ++iter) {
                        d = index->getDist()->compare(index->getBaseData() + qnode->GetId() * index->getBaseDim(),
                                                      index->getBaseData() + (*iter)->GetId() * index->getBaseDim(),
                                                      index->getBaseDim());

                        if (d < cur_dist) {
                            cur_dist = d;
                            cur_node = *iter;
                            changed = true;
                        }
                    }
                }
            }
            enterpoint = cur_node;
        }

        // PRUNE
        ComponentPrune *a = new ComponentPruneHeuristic(index);

        for (auto i = std::min(max_level_copy, cur_level); i >= 0; --i) {
            std::priority_queue<Index::FurtherFirst> result;
            SearchAtLayer(qnode, enterpoint, i, visited_list, result);

            a->Hnsw2Neighbor(index->m_, result);

            while (!result.empty()) {
                auto *top_node = result.top().GetNode();
                result.pop();
                Link(top_node, qnode, i);
                Link(qnode, top_node, i);
            }
        }
        if (cur_level > index->enterpoint_->GetLevel()) {
            index->enterpoint_ = qnode;
            index->max_level_ = cur_level;
        }
    }

    void ComponentInitHNSW::SearchAtLayer(Index::HnswNode *qnode, Index::HnswNode *enterpoint, int level,
                                          Index::VisitedList *visited_list,
                                          std::priority_queue<Index::FurtherFirst> &result) {
        // TODO: check Node 12bytes => 8bytes
        std::priority_queue<Index::CloserFirst> candidates;
        float d = index->getDist()->compare(index->getBaseData() + qnode->GetId() * index->getBaseDim(),
                                            index->getBaseData() + enterpoint->GetId() * index->getBaseDim(),
                                            index->getBaseDim());
        result.emplace(enterpoint, d);
        candidates.emplace(enterpoint, d);

        visited_list->Reset();
        visited_list->MarkAsVisited(enterpoint->GetId());

        while (!candidates.empty()) {
            const Index::CloserFirst &candidate = candidates.top();
            float lower_bound = result.top().GetDistance();
            if (candidate.GetDistance() > lower_bound)
                break;

            Index::HnswNode *candidate_node = candidate.GetNode();
            std::unique_lock<std::mutex> lock(candidate_node->GetAccessGuard());
            const std::vector<Index::HnswNode *> &neighbors = candidate_node->GetFriends(level);
            candidates.pop();

            for (const auto &neighbor : neighbors) {
                int id = neighbor->GetId();
                if (visited_list->NotVisited(id)) {
                    visited_list->MarkAsVisited(id);
                    d = index->getDist()->compare(index->getBaseData() + qnode->GetId() * index->getBaseDim(),
                                                  index->getBaseData() + neighbor->GetId() * index->getBaseDim(),
                                                  index->getBaseDim());
                    if (result.size() < index->ef_construction_ || result.top().GetDistance() > d) {
                        result.emplace(neighbor, d);
                        candidates.emplace(neighbor, d);
                        if (result.size() > index->ef_construction_)
                            result.pop();
                    }
                }
            }
        }
    }

    void ComponentInitHNSW::Link(Index::HnswNode *source, Index::HnswNode *target, int level) {
        std::unique_lock<std::mutex> lock(source->GetAccessGuard());
        std::vector<Index::HnswNode *> &neighbors = source->GetFriends(level);
        neighbors.push_back(target);
        bool shrink = (level > 0 && neighbors.size() > source->GetMaxM()) ||
                      (level <= 0 && neighbors.size() > source->GetMaxM0());
        //std::cout << "shrink : " << shrink << std::endl;
        if (!shrink) return;

//        float max = index->getDist()->compare(index->getBaseData() + source->GetId() * index->getBaseDim(),
//                                              index->getBaseData() + neighbors[0]->GetId() * index->getBaseDim(),
//                                              index->getBaseDim());
//        int maxi = 0;
//        for(size_t i = 1; i < neighbors.size(); i ++) {
//            float curd = index->getDist()->compare(index->getBaseData() + source->GetId() * index->getBaseDim(),
//                                                   index->getBaseData() + neighbors[i]->GetId() * index->getBaseDim(),
//                                                   index->getBaseDim());
//
//            if(curd > max) {
//                max = curd;
//                maxi = i;
//            }
//        }
//        neighbors.erase(neighbors.begin() + maxi);

        std::priority_queue<Index::FurtherFirst> tempres;
//            for (const auto& neighbor : neighbors) {
//                _mm_prefetch(neighbor->GetData(), _MM_HINT_T0);
//            }
        //std::cout << neighbors.size() << std::endl;
        for (const auto &neighbor : neighbors) {
            //std::cout << "neighbors : " << neighbor->GetId() << std::endl;
            float tmp = index->getDist()->compare(index->getBaseData() + source->GetId() * index->getBaseDim(),
                                                  index->getBaseData() + neighbor->GetId() * index->getBaseDim(),
                                                  index->getBaseDim());
//            std::cout << tmp << std::endl;
//            std::cout << neighbor->GetId() << std::endl;
//            std::cout << tempres.size() << std::endl;
//            if(!tempres.empty())
//                std::cout << tempres.top().GetNode()->GetId() << std::endl;
            tempres.push(Index::FurtherFirst(neighbors[0], tmp));
//            std::cout << "mm" << std::endl;
        }
//        std::cout << "tempres : " << tempres.size() << std::endl;

        // PRUNE
        ComponentPrune *a = new ComponentPruneHeuristic(index);
        a->Hnsw2Neighbor(tempres.size() - 1, tempres);

        //std::cout << "ff" << tempres.size() << std::endl;
        neighbors.clear();
        while (tempres.size()) {
            neighbors.emplace_back(tempres.top().GetNode());
            tempres.pop();
        }
        std::priority_queue<Index::FurtherFirst>().swap(tempres);
    }


    // ANNG
    void ComponentInitANNG::InitInner() {
        SetConfigs();

        Build();
    }

    void ComponentInitANNG::SetConfigs() {
        index->edgeSizeForCreation = index->getParam().get<unsigned>("edgeSizeForCreation");
        index->truncationThreshold = index->getParam().get<unsigned>("truncationThreshold");
        index->edgeSizeForSearch = index->getParam().get<unsigned>("edgeSizeForSearch");

        index->size = index->getParam().get<unsigned>("size");
    }

    void ComponentInitANNG::Build() {
        index->getFinalGraph().resize(index->getBaseLen());

        // 为插入操作提前计算距离
        for (unsigned idxi = 0; idxi < index->getBaseLen(); idxi++) {
            std::vector<Index::SimpleNeighbor> tmp;
            for (unsigned idxj = 0; idxj < idxi; idxj++) {
                float d = index->getDist()->compare(index->getBaseData() + idxi * index->getBaseDim(),
                                                    index->getBaseData() + idxj * index->getBaseDim(),
                                                    index->getBaseDim());
                tmp.emplace_back(idxj, d);
            }
            std::sort(tmp.begin(), tmp.end());
            if (tmp.size() > index->edgeSizeForCreation) {
                tmp.resize(index->edgeSizeForCreation);
            }

            index->getFinalGraph()[idxi] = tmp;
        }

        // 逐个进行插入操作
        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            InsertNode(i);
        }
    }

    void ComponentInitANNG::InsertNode(unsigned id) {
        std::queue<unsigned> truncateQueue;

        for (unsigned i = 0; i < index->getFinalGraph()[id].size(); i++) {
            assert(index->getFinalGraph()[id][i].id != id);

            if (addEdge(index->getFinalGraph()[id][i].id, id, index->getFinalGraph()[id][i].distance)) {
                truncateQueue.push(index->getFinalGraph()[id][i].id);
            }
        }

        while (!truncateQueue.empty()) {
            unsigned tid = truncateQueue.front();
            truncateEdgesOptimally(tid, index->edgeSizeForCreation);
            truncateQueue.pop();
        }
    }

    bool ComponentInitANNG::addEdge(unsigned target, unsigned addID, float dist) {
        Index::SimpleNeighbor obj(addID, dist);

        auto ni = std::lower_bound(index->getFinalGraph()[target].begin(), index->getFinalGraph()[target].end(), obj);
        if ((ni != index->getFinalGraph()[target].end()) && (ni->id == addID)) {
            std::cout << "NGT::addEdge: already existed! " << ni->id << ":" << addID << std::endl;
        } else {
            index->getFinalGraph()[target].insert(ni, obj);
        }

        if (index->truncationThreshold != 0 && index->getFinalGraph()[target].size() > index->truncationThreshold) {
            return true;
        }
        return false;
    }

    void ComponentInitANNG::truncateEdgesOptimally(unsigned id, size_t truncationSize) {
        std::vector<Index::SimpleNeighbor> delNodes;
        size_t osize = index->getFinalGraph()[id].size();

        for (size_t i = truncationSize; i < osize; i++) {
            if (id == index->getFinalGraph()[id][i].id) {
                continue;
            }
            delNodes.push_back(index->getFinalGraph()[id][i]);
        }

        auto ri = index->getFinalGraph()[id].begin();
        ri += truncationSize;
        index->getFinalGraph()[id].erase(ri, index->getFinalGraph()[id].end());

        for (size_t i = 0; i < delNodes.size(); i++) {
            for (auto j = index->getFinalGraph()[delNodes[i].id].begin();
                 j != index->getFinalGraph()[delNodes[i].id].end(); j++) {
                if ((*j).id == id) {
                    index->getFinalGraph()[delNodes[i].id].erase(j);
                    break;
                }
            }
        }

        for (unsigned i = 0; i < delNodes.size(); i++) {
            std::vector<Index::SimpleNeighbor> pool;
            Search(id, delNodes[i].id, pool);

            Index::SimpleNeighbor nearest = pool.front();
            if (nearest.id != delNodes[i].id) {
                unsigned tid = delNodes[i].id;
                auto iter = std::lower_bound(index->getFinalGraph()[tid].begin(), index->getFinalGraph()[tid].end(),
                                             nearest);
                if ((*iter).id != nearest.id) {
                    index->getFinalGraph()[tid].insert(iter, nearest);
                }

                Index::SimpleNeighbor obj(tid, delNodes[i].distance);
                index->getFinalGraph()[nearest.id].push_back(obj);
                std::sort(index->getFinalGraph()[nearest.id].begin(), index->getFinalGraph()[nearest.id].end());
            }
        }
    }

    void ComponentInitANNG::Search(unsigned startId, unsigned query, std::vector<Index::SimpleNeighbor> &pool) {
        unsigned edgeSize = index->edgeSizeForSearch;
        float radius = 3.402823466e+38F;
        float explorationRadius = index->explorationCoefficient * radius;

        // 大顶堆
        std::priority_queue<Index::SimpleNeighbor, std::vector<Index::SimpleNeighbor>, std::less<Index::SimpleNeighbor>> result;
        std::priority_queue<Index::SimpleNeighbor, std::vector<Index::SimpleNeighbor>, std::greater<Index::SimpleNeighbor>> unchecked;
        std::unordered_set<unsigned> distanceChecked;

        float d = index->getDist()->compare(index->getBaseData() + index->getBaseDim() * startId,
                                            index->getBaseData() + index->getBaseDim() * query,
                                            index->getBaseDim());
        Index::SimpleNeighbor obj(startId, d);
//        unchecked.push(obj);      //bug here
//        result.push(obj);
//        distanceChecked.insert(startId);
//
//        while (!unchecked.empty()) {
//            Index::SimpleNeighbor target = unchecked.top();
//            unchecked.pop();
//
//            if (target.distance > explorationRadius) {
//                break;
//            }
//
//            if (index->getFinalGraph()[target.id].empty()) continue;
//            unsigned neighborSize = index->getFinalGraph()[target.id].size() < edgeSize ?
//                                    index->getFinalGraph()[target.id].size() : edgeSize;
//
//            for (int i = 0; i < neighborSize; i++) {
//                if (distanceChecked.find(index->getFinalGraph()[target.id][i].id) != distanceChecked.end())
//                    continue;
//
//                distanceChecked.insert(index->getFinalGraph()[target.id][i].id);
//                float dist = index->getDist()->compare(
//                        index->getBaseData() + index->getBaseDim() * index->getFinalGraph()[target.id][i].id,
//                        index->getBaseData() + index->getBaseDim() * query,
//                        index->getBaseDim());
//                if (dist <= explorationRadius) {
//                    unchecked.push(Index::SimpleNeighbor(index->getFinalGraph()[target.id][i].id, dist));
//                    if (dist <= radius) {
//                        result.push(Index::SimpleNeighbor(index->getFinalGraph()[target.id][i].id, dist));
//                        if (result.size() > index->size) {
//                            if (result.top().distance >= dist) {
//                                if (result.size() > index->size) {
//                                    result.pop();
//                                }
//                                radius = result.top().distance;
//                                explorationRadius = index->explorationCoefficient * radius;
//                            }
//                        }
//                    }
//                }
//            }
//        }
//
//        for (int i = 0; i < result.size(); i++) {
//            pool.push_back(result.top());
//            result.pop();
//        }
//        std::sort(pool.begin(), pool.end());
    }


    // SPTAG
    unsigned ComponentInitSPTAG::rand(unsigned high, unsigned low) {
        return low + (unsigned) (float(high - low) * (std::rand() / (RAND_MAX + 1.0)));
    }

    void ComponentInitSPTAG::BuildGraph() {
        index->m_iNeighborhoodSize = index->m_iNeighborhoodSize * index->m_iNeighborhoodScale;       // L

        index->getFinalGraph().resize(index->getBaseLen());

        std::vector<std::vector<unsigned>> TptreeDataIndices(index->m_iTPTNumber,
                                                             std::vector<unsigned>(index->getBaseLen()));
        std::vector<std::vector<std::pair<unsigned, unsigned>>> TptreeLeafNodes(index->m_iTPTNumber,
                                                                                std::vector<std::pair<unsigned, unsigned>>());

        float MaxDist = (std::numeric_limits<float>::max)();
        float MaxId = (std::numeric_limits<unsigned>::max)();

        for (unsigned i = 0; i < index->getBaseLen(); i++) {
            index->getFinalGraph()[i].resize(index->m_iNeighborhoodSize);
            for (unsigned j = 0; j < index->m_iNeighborhoodSize; j++) {
                Index::SimpleNeighbor neighbor(MaxId, MaxDist);
                index->getFinalGraph()[i][j] = neighbor;
            }
        }

#pragma omp parallel for schedule(dynamic)
        for (int i = 0; i < index->m_iTPTNumber; i++) {
            // 非多线程注意注释
            Sleep(i * 100);
            std::srand(clock());
            for (unsigned j = 0; j < index->getBaseLen(); j++) TptreeDataIndices[i][j] = j;
            std::random_shuffle(TptreeDataIndices[i].begin(), TptreeDataIndices[i].end());
            PartitionByTptree(TptreeDataIndices[i], 0, index->getBaseLen() - 1, TptreeLeafNodes[i]);
            std::cout << "Finish Getting Leaves for Tree : " << i << std::endl;
        }
        std::cout << "Parallel TpTree Partition done" << std::endl;

        for (int i = 0; i < index->m_iTPTNumber; i++) {
#pragma omp parallel for schedule(dynamic)
            for (unsigned j = 0; j < (unsigned) TptreeLeafNodes[i].size(); j++) {
                unsigned start_index = TptreeLeafNodes[i][j].first;
                unsigned end_index = TptreeLeafNodes[i][j].second;
                if ((j * 5) % TptreeLeafNodes[i].size() == 0)
                    std::cout << "Processing Tree : " << i
                              << static_cast<int>(j * 1.0 / TptreeLeafNodes[i].size() * 100) << std::endl;

                for (unsigned x = start_index; x < end_index; x++) {
                    for (unsigned y = x + 1; y <= end_index; y++) {
                        unsigned p1 = TptreeDataIndices[i][x];
                        unsigned p2 = TptreeDataIndices[i][y];
                        float dist = index->getDist()->compare(index->getBaseData() + index->getBaseDim() * p1,
                                                               index->getBaseData() + index->getBaseDim() * p2,
                                                               index->getBaseDim());

                        AddNeighbor(p2, dist, p1, index->m_iNeighborhoodSize);
                        AddNeighbor(p1, dist, p2, index->m_iNeighborhoodSize);
                    }
                }
            }
            TptreeDataIndices[i].clear();
            TptreeLeafNodes[i].clear();
        }
        TptreeDataIndices.clear();
        TptreeLeafNodes.clear();
    }

    inline bool ComponentInitSPTAG::Compare(const Index::SimpleNeighbor &lhs, const Index::SimpleNeighbor &rhs) {
        return ((lhs.distance < rhs.distance) || ((lhs.distance == rhs.distance) && (lhs.id < rhs.id)));
    }

    void
    ComponentInitSPTAG::PartitionByTptree(std::vector<unsigned> &indices, const unsigned first, const unsigned last,
                                              std::vector<std::pair<unsigned, unsigned>> &leaves) {
        // 叶子结点个数
        unsigned m_iTPTLeafSize = 2000;
        unsigned m_numTopDimensionTPTSplit = 5;

        if (last - first <= m_iTPTLeafSize) {
            leaves.emplace_back(first, last);
        } else {
            std::vector<float> Mean(index->getBaseDim(), 0);

            int iIteration = 100;
            unsigned end = std::min(first + index->m_iSamples, last);
            unsigned count = end - first + 1;
            // calculate the mean of each dimension
            for (unsigned j = first; j <= end; j++) {
                const float *v = index->getBaseData() + indices[j] * index->getBaseDim();
                for (unsigned k = 0; k < index->getBaseDim(); k++) {
                    Mean[k] += v[k];
                }
            }
            // 计算每个维度平均值
            for (unsigned k = 0; k < index->getBaseDim(); k++) {
                Mean[k] /= count;
            }
            std::vector<Index::SimpleNeighbor> Variance;
            Variance.reserve(index->getBaseDim());
            for (unsigned j = 0; j < index->getBaseDim(); j++) {
                Variance.emplace_back(j, 0.0f);
            }
            // calculate the variance of each dimension
            for (unsigned j = first; j <= end; j++) {
                const float *v = index->getBaseData() + index->getBaseDim() * indices[j];
                for (unsigned k = 0; k < index->getBaseDim(); k++) {
                    float dist = v[k] - Mean[k];
                    Variance[k].distance += dist * dist;
                }
            }
            std::sort(Variance.begin(), Variance.end(), ComponentInitSPTAG::Compare);
            std::vector<unsigned> indexs(m_numTopDimensionTPTSplit);
            std::vector<float> weight(m_numTopDimensionTPTSplit), bestweight(m_numTopDimensionTPTSplit);
            float bestvariance = Variance[index->getBaseDim() - 1].distance;
            // 选出离散程度更大的 m_numTopDimensionTPTSplit 个维度
            for (int i = 0; i < m_numTopDimensionTPTSplit; i++) {
                indexs[i] = Variance[index->getBaseDim() - 1 - i].id;
                bestweight[i] = 0;
            }
            bestweight[0] = 1;
            float bestmean = Mean[indexs[0]];

            std::vector<float> Val(count);
            for (int i = 0; i < iIteration; i++) {
                float sumweight = 0;
                for (int j = 0; j < m_numTopDimensionTPTSplit; j++) {
                    weight[j] = float(std::rand() % 10000) / 5000.0f - 1.0f;
                    sumweight += weight[j] * weight[j];
                }
                sumweight = sqrt(sumweight);
                for (int j = 0; j < m_numTopDimensionTPTSplit; j++) {
                    weight[j] /= sumweight;
                }
                float mean = 0;
                for (unsigned j = 0; j < count; j++) {
                    Val[j] = 0;
                    const float *v = index->getBaseData() + index->getBaseDim() * indices[first + j];
                    for (int k = 0; k < m_numTopDimensionTPTSplit; k++) {
                        Val[j] += weight[k] * v[indexs[k]];
                    }
                    mean += Val[j];
                }
                mean /= count;
                float var = 0;
                for (unsigned j = 0; j < count; j++) {
                    float dist = Val[j] - mean;
                    var += dist * dist;
                }
                if (var > bestvariance) {
                    bestvariance = var;
                    bestmean = mean;
                    for (int j = 0; j < m_numTopDimensionTPTSplit; j++) {
                        bestweight[j] = weight[j];
                    }
                }
            }
            unsigned i = first;
            unsigned j = last;
            // decide which child one point belongs
            while (i <= j) {
                float val = 0;
                const float *v = index->getBaseData() + index->getBaseDim() * indices[i];
                for (int k = 0; k < m_numTopDimensionTPTSplit; k++) {
                    val += bestweight[k] * v[indexs[k]];
                }
                if (val < bestmean) {
                    i++;
                } else {
                    std::swap(indices[i], indices[j]);
                    j--;
                }
            }
            // if all the points in the node are equal,equally split the node into 2
            if ((i == first) || (i == last + 1)) {
                i = (first + last + 1) / 2;
            }

            Mean.clear();
            Variance.clear();
            Val.clear();
            indexs.clear();
            weight.clear();
            bestweight.clear();

            PartitionByTptree(indices, first, i - 1, leaves);
            PartitionByTptree(indices, i, last, leaves);
        }
    }

    void ComponentInitSPTAG::AddNeighbor(unsigned idx, float dist, unsigned origin, unsigned size) {
        size--;
        if (dist < index->getFinalGraph()[origin][size].distance ||
            (dist == index->getFinalGraph()[origin][size].distance && idx < index->getFinalGraph()[origin][size].id)) {
            unsigned nb;

            for (nb = 0; nb <= size && index->getFinalGraph()[origin][nb].id != idx; nb++);

            if (nb > size) {
                nb = size;
                while (nb > 0 && (dist < index->getFinalGraph()[origin][nb - 1].distance ||
                                  (dist == index->getFinalGraph()[origin][nb - 1].distance &&
                                   idx < index->getFinalGraph()[origin][nb - 1].id))) {
                    index->getFinalGraph()[origin][nb] = index->getFinalGraph()[origin][nb - 1];
                    nb--;
                }
                index->getFinalGraph()[origin][nb].distance = dist;
                index->getFinalGraph()[origin][nb].id = idx;
            }
        }
    }


    // SPTAG KDT
    void ComponentInitSPTAG_KDT::InitInner() {
        SetConfigs();

        BuildTrees();

        BuildGraph();
    }

    void ComponentInitSPTAG_KDT::SetConfigs() {
        index->numOfThreads = index->getParam().get<unsigned>("numOfThreads");

        index->m_iTreeNumber = 2;
    }

    void ComponentInitSPTAG_KDT::BuildTrees() {
        std::vector<unsigned> localindices;
        localindices.resize(index->getBaseLen());
        for (unsigned i = 0; i < localindices.size(); i++) localindices[i] = i;

        // 记录 KDT 结构
        index->m_pKDTreeRoots.resize(index->m_iTreeNumber * localindices.size());
        // 记录树根
        index->m_pTreeStart.resize(index->m_iTreeNumber, 0);

#pragma omp parallel for num_threads(index->numOfThreads)
        for (int i = 0; i < index->m_iTreeNumber; i++) {
            // 非多线程 -> 删除 ！！！
            Sleep(i * 100);
            std::srand(clock());

            std::vector<unsigned> pindices(localindices.begin(), localindices.end());
            std::random_shuffle(pindices.begin(), pindices.end());

            index->m_pTreeStart[i] = i * pindices.size();
            std::cout << "Start to build KDTree " << i + 1 << std::endl;
            unsigned iTreeSize = index->m_pTreeStart[i];
            DivideTree(pindices, 0, pindices.size() - 1, index->m_pTreeStart[i], iTreeSize);
            std::cout << i + 1 << " KDTree built, " << iTreeSize - index->m_pTreeStart[i] << " " << pindices.size()
                      << std::endl;
        }
    }

    void ComponentInitSPTAG_KDT::DivideTree(std::vector<unsigned> &indices, unsigned first, unsigned last,
                                            unsigned tree_index, unsigned &iTreeSize) {
        // 选择分离维度
        ChooseDivision(index->m_pKDTreeRoots[tree_index], indices, first, last);
        unsigned i = Subdivide(index->m_pKDTreeRoots[tree_index], indices, first, last);
        if (i - 1 <= first) {
            index->m_pKDTreeRoots[tree_index].left = -indices[first] - 1;
        } else {
            iTreeSize++;
            index->m_pKDTreeRoots[tree_index].left = iTreeSize;
            DivideTree(indices, first, i - 1, iTreeSize, iTreeSize);
        }
        if (last == i) {
            index->m_pKDTreeRoots[tree_index].right = -indices[last] - 1;
        } else {
            iTreeSize++;
            index->m_pKDTreeRoots[tree_index].right = iTreeSize;
            DivideTree(indices, i, last, iTreeSize, iTreeSize);
        }
    }

    void ComponentInitSPTAG_KDT::ChooseDivision(Index::KDTNode &node, const std::vector<unsigned> &indices,
                                                const unsigned first, const unsigned last) {

        std::vector<float> meanValues(index->getBaseDim(), 0);
        std::vector<float> varianceValues(index->getBaseDim(), 0);
        unsigned end = std::min(first + index->m_iSamples, last);
        unsigned count = end - first + 1;
        // calculate the mean of each dimension
        for (unsigned j = first; j <= end; j++) {
            float *v = index->getBaseData() + index->getBaseDim() * indices[j];
            for (unsigned k = 0; k < index->getBaseDim(); k++) {
                meanValues[k] += v[k];
            }
        }
        for (unsigned k = 0; k < index->getBaseDim(); k++) {
            meanValues[k] /= count;
        }
        // calculate the variance of each dimension
        for (unsigned j = first; j <= end; j++) {
            const float *v = index->getBaseData() + index->getBaseDim() * indices[j];
            for (unsigned k = 0; k < index->getBaseDim(); k++) {
                float dist = v[k] - meanValues[k];
                varianceValues[k] += dist * dist;
            }
        }
        // choose the split dimension as one of the dimension inside TOP_DIM maximum variance
        node.split_dim = SelectDivisionDimension(varianceValues);
        // determine the threshold
        node.split_value = meanValues[node.split_dim];
    }

    unsigned ComponentInitSPTAG_KDT::SelectDivisionDimension(const std::vector<float> &varianceValues) {
        unsigned m_numTopDimensionKDTSplit = 5;

        // Record the top maximum variances
        std::vector<unsigned> topind(m_numTopDimensionKDTSplit);
        int num = 0;
        // order the variances
        for (unsigned i = 0; i < varianceValues.size(); i++) {
            if (num < m_numTopDimensionKDTSplit || varianceValues[i] > varianceValues[topind[num - 1]]) {
                if (num < m_numTopDimensionKDTSplit) {
                    topind[num++] = i;
                } else {
                    topind[num - 1] = i;
                }
                int j = num - 1;
                // order the TOP_DIM variances
                while (j > 0 && varianceValues[topind[j]] > varianceValues[topind[j - 1]]) {
                    std::swap(topind[j], topind[j - 1]);
                    j--;
                }
            }
        }
        // randomly choose a dimension from TOP_DIM
        return topind[rand(num)];
    }

    unsigned
    ComponentInitSPTAG_KDT::Subdivide(const Index::KDTNode &node, std::vector<unsigned> &indices, const unsigned first,
                                      const unsigned last) {
        unsigned i = first;
        unsigned j = last;
        // decide which child one point belongs
        while (i <= j) {
            unsigned ind = indices[i];
            const float *v = index->getBaseData() + index->getBaseDim() * ind;
            float val = v[node.split_dim];
            if (val < node.split_value) {
                i++;
            } else {
                std::swap(indices[i], indices[j]);
                j--;
            }
        }
        // if all the points in the node are equal,equally split the node into 2
        if ((i == first) || (i == last + 1)) {
            i = (first + last + 1) / 2;
        }
        return i;
    }


    // SPTAG_BKT
    void ComponentInitSPTAG_BKT::InitInner() {
        SetConfigs();

        BuildTrees();

        BuildGraph();
    }

    void ComponentInitSPTAG_BKT::SetConfigs() {
        index->numOfThreads = index->getParam().get<unsigned>("numOfThreads");

        index->m_iTreeNumber = 1;
    }

    void ComponentInitSPTAG_BKT::BuildTrees() {
        struct BKTStackItem {
            unsigned index, first, last;

            BKTStackItem(unsigned index_, unsigned first_, unsigned last_) : index(index_), first(first_),
                                                                             last(last_) {}
        };
        std::stack<BKTStackItem> ss;

        std::vector<unsigned> localindices;
        localindices.resize(index->getBaseLen());
        for (unsigned i = 0; i < localindices.size(); i++) localindices[i] = i;

        Index::KmeansArgs<float> args(index->m_iBKTKmeansK, index->getBaseDim(), localindices.size(),
                                      index->numOfThreads);

        index->m_pSampleCenterMap.clear();

        unsigned m_iBKTLeafSize = 8;
        for (char i = 0; i < index->m_iTreeNumber; i++) {
            std::random_shuffle(localindices.begin(), localindices.end());

            index->m_pTreeStart.push_back(index->m_pBKTreeRoots.size());
            index->m_pBKTreeRoots.emplace_back(localindices.size());
            std::cout << "Start to build BKTree : " << i + 1 << std::endl;

            ss.push(BKTStackItem(index->m_pTreeStart[i], 0, localindices.size()));
            while (!ss.empty()) {
                BKTStackItem item = ss.top();
                ss.pop();
                unsigned newBKTid = (unsigned) index->m_pBKTreeRoots.size();
                index->m_pBKTreeRoots[item.index].childStart = newBKTid;
                if (item.last - item.first <= m_iBKTLeafSize) {
                    for (unsigned j = item.first; j < item.last; j++) {
                        unsigned cid = localindices[j];
                        index->m_pBKTreeRoots.emplace_back(cid);
                    }
                } else { // clustering the data into BKTKmeansK clusters

                    int numClusters = KmeansClustering(localindices, item.first, item.last, args, index->m_iSamples);
                    if (numClusters <= 1) {
                        unsigned end = std::min(item.last + 1, (unsigned) localindices.size());
                        std::sort(localindices.begin() + item.first, localindices.begin() + end);
                        index->m_pBKTreeRoots[item.index].centerid = localindices[item.first];
                        index->m_pBKTreeRoots[item.index].childStart = -index->m_pBKTreeRoots[item.index].childStart;
                        for (unsigned j = item.first + 1; j < end; j++) {
                            unsigned cid = localindices[j];
                            index->m_pBKTreeRoots.emplace_back(cid);
                            index->m_pSampleCenterMap[cid] = index->m_pBKTreeRoots[item.index].centerid;
                        }
                        index->m_pSampleCenterMap[-1 - index->m_pBKTreeRoots[item.index].centerid] = item.index;
                    } else {
                        for (int k = 0; k < index->m_iBKTKmeansK; k++) {
                            if (args.counts[k] == 0) continue;
                            unsigned cid = localindices[item.first + args.counts[k] - 1];
                            index->m_pBKTreeRoots.emplace_back(cid);
                            if (args.counts[k] > 1)
                                ss.push(BKTStackItem(newBKTid++, item.first, item.first + args.counts[k] - 1));
                            item.first += args.counts[k];
                        }
                    }
                }
                index->m_pBKTreeRoots[item.index].childEnd = (unsigned) index->m_pBKTreeRoots.size();
            }
            index->m_pBKTreeRoots.emplace_back(-1);
            std::cout << i + 1 << " BKTree built, " << index->m_pBKTreeRoots.size() - index->m_pTreeStart[i] << " "
                      << localindices.size() << std::endl;
        }
    }

    int
    ComponentInitSPTAG_BKT::KmeansClustering(std::vector<unsigned> &indices, const unsigned first, const unsigned last,
                                             Index::KmeansArgs<float> &args, int samples) {

        const float MaxDist = (std::numeric_limits<float>::max)();

        InitCenters(indices, first, last, args, samples, 3);

        unsigned batchEnd = std::min(first + samples, last);
        float currDiff, currDist, minClusterDist = MaxDist;
        int noImprovement = 0;
        for (int iter = 0; iter < 100; iter++) {
            std::memcpy(args.centers, args.newTCenters, sizeof(float) * args._K * args._D);
            std::random_shuffle(indices.begin() + first, indices.begin() + last);

            args.ClearCenters();
            args.ClearCounts();
            args.ClearDists(-MaxDist);
            // ?
            currDist = KmeansAssign(indices, first, batchEnd, args, true, 1 / (100.0f * (batchEnd - first)));
            std::memcpy(args.counts, args.newCounts, sizeof(unsigned) * args._K);

            if (currDist < minClusterDist) {
                noImprovement = 0;
                minClusterDist = currDist;
            } else {
                noImprovement++;
            }
            currDiff = RefineCenters(args);
            if (currDiff < 1e-3 || noImprovement >= 5) break;
        }

        args.ClearCounts();
        args.ClearDists(MaxDist);
        currDist = KmeansAssign(indices, first, last, args, false, 0);
        std::memcpy(args.counts, args.newCounts, sizeof(unsigned) * args._K);

        int numClusters = 0;
        for (int i = 0; i < args._K; i++) if (args.counts[i] > 0) numClusters++;

        if (numClusters <= 1) {
            return numClusters;
        }
        args.Shuffle(indices, first, last);
        return numClusters;
    }

    float ComponentInitSPTAG_BKT::RefineCenters(Index::KmeansArgs<float> &args) {
        int maxcluster = -1;
        unsigned maxCount = 0;

        for (int k = 0; k < args._DK; k++) {
            float dist = index->getDist()->compare(index->getBaseData() + index->getBaseDim() * args.clusterIdx[k],
                                                  args.centers + k * args._D,
                                                  index->getBaseDim());
            if (args.counts[k] > maxCount && args.newCounts[k] > 0
                && dist > 1e-6) {
                maxcluster = k;
                maxCount = args.counts[k];
            }
        }

        if (maxcluster != -1 && (args.clusterIdx[maxcluster] < 0 || args.clusterIdx[maxcluster] >= index->getBaseLen()))
            std::cout << "maxcluster:" << maxcluster << "(" << args.newCounts[maxcluster] << ")" << " Error dist:"
                      << args.clusterDist[maxcluster] << std::endl;

        float diff = 0;
        for (int k = 0; k < args._DK; k++) {
            float *TCenter = args.newTCenters + k * args._D;
            if (args.counts[k] == 0) {
                if (maxcluster != -1) {
                    //int nextid = Utils::rand_int(last, first);
                    //while (args.label[nextid] != maxcluster) nextid = Utils::rand_int(last, first);
                    unsigned nextid = args.clusterIdx[maxcluster];
                    std::memcpy(TCenter, index->getBaseData() + index->getBaseDim() * nextid, sizeof(float) * args._D);
                } else {
                    std::memcpy(TCenter, args.centers + k * args._D, sizeof(float) * args._D);
                }
            } else {
                float *currCenters = args.newCenters + k * args._D;
                for (unsigned j = 0; j < args._D; j++) currCenters[j] /= args.counts[k];

                for (unsigned j = 0; j < args._D; j++) TCenter[j] = (float) (currCenters[j]);
            }
            diff += args.fComputeDistance(args.centers + k * args._D, TCenter, args._D);
        }
        return diff;
    }

    inline float ComponentInitSPTAG_BKT::KmeansAssign(std::vector<unsigned> &indices,
                                                      const unsigned first, const unsigned last,
                                                      Index::KmeansArgs<float> &args,
                                                      const bool updateCenters, float lambda) {
        const float MaxDist = (std::numeric_limits<float>::max)();
        float currDist = 0;
        unsigned subsize = (last - first - 1) / args._T + 1;

        //并行已删除
        for (int tid = 0; tid < args._T; tid++) {
            unsigned istart = first + tid * subsize;
            unsigned iend = std::min(first + (tid + 1) * subsize, last);
            unsigned *inewCounts = args.newCounts + tid * args._K;
            float *inewCenters = args.newCenters + tid * args._K * args._D;
            unsigned *iclusterIdx = args.clusterIdx + tid * args._K;
            float *iclusterDist = args.clusterDist + tid * args._K;
            float idist = 0;
            for (unsigned i = istart; i < iend; i++) {
                int clusterid = 0;
                float smallestDist = MaxDist;
                for (int k = 0; k < args._DK; k++) {
                    float dist = index->getDist()->compare(index->getBaseData() + index->getBaseDim() * indices[i],
                                                           args.centers + k * args._D,
                                                           index->getBaseDim()) + lambda * args.counts[k];
                    if (dist > -MaxDist && dist < smallestDist) {
                        clusterid = k;
                        smallestDist = dist;
                    }
                }
                args.label[i] = clusterid;
                inewCounts[clusterid]++;
                idist += smallestDist;
                if (updateCenters) {
                    const float *v = index->getBaseData() + index->getBaseDim() * indices[i];
                    float *center = inewCenters + clusterid * args._D;
                    for (unsigned j = 0; j < args._D; j++) center[j] += v[j];
                    if (smallestDist > iclusterDist[clusterid]) {
                        iclusterDist[clusterid] = smallestDist;
                        iclusterIdx[clusterid] = indices[i];
                    }
                } else {
                    if (smallestDist <= iclusterDist[clusterid]) {
                        iclusterDist[clusterid] = smallestDist;
                        iclusterIdx[clusterid] = indices[i];
                    }
                }
            }
            currDist += idist;
        }

        for (int i = 1; i < args._T; i++) {
            for (int k = 0; k < args._DK; k++)
                args.newCounts[k] += args.newCounts[i * args._K + k];
        }

        if (updateCenters) {
            for (int i = 1; i < args._T; i++) {
                float *currCenter = args.newCenters + i * args._K * args._D;
                for (size_t j = 0; j < ((size_t) args._DK) * args._D; j++) args.newCenters[j] += currCenter[j];

                for (int k = 0; k < args._DK; k++) {
                    if (args.clusterIdx[i * args._K + k] != -1 &&
                        args.clusterDist[i * args._K + k] > args.clusterDist[k]) {
                        args.clusterDist[k] = args.clusterDist[i * args._K + k];
                        args.clusterIdx[k] = args.clusterIdx[i * args._K + k];
                    }
                }
            }
        } else {
            for (int i = 1; i < args._T; i++) {
                for (int k = 0; k < args._DK; k++) {
                    if (args.clusterIdx[i * args._K + k] != -1 &&
                        args.clusterDist[i * args._K + k] <= args.clusterDist[k]) {
                        args.clusterDist[k] = args.clusterDist[i * args._K + k];
                        args.clusterIdx[k] = args.clusterIdx[i * args._K + k];
                    }
                }
            }
        }
        return currDist;
    }

    inline void
    ComponentInitSPTAG_BKT::InitCenters(std::vector<unsigned> &indices, const unsigned first, const unsigned last,
                                        Index::KmeansArgs<float> &args, int samples, int tryIters) {
        const float MaxDist = (std::numeric_limits<float>::max)();
        unsigned batchEnd = std::min(first + samples, last);
        float currDist, minClusterDist = MaxDist;
        for (int numKmeans = 0; numKmeans < tryIters; numKmeans++) {
            for (int k = 0; k < args._DK; k++) {
                unsigned randid = rand(last, first);
                std::memcpy(args.centers + k * args._D, index->getBaseData() + index->getBaseDim() * indices[randid],
                            sizeof(float) * args._D);
            }
            args.ClearCounts();
            args.ClearDists(MaxDist);
            currDist = KmeansAssign(indices, first, batchEnd, args, false, 0);
            if (currDist < minClusterDist) {
                minClusterDist = currDist;
                memcpy(args.newTCenters, args.centers, sizeof(float) * args._K * args._D);
                memcpy(args.counts, args.newCounts, sizeof(unsigned) * args._K);
            }
        }
    }


}
