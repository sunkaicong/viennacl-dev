#ifndef VIENNACL_DEVICE_SPECIFIC_TEMPLATES_MATRIX_PRODUCT_HPP
#define VIENNACL_DEVICE_SPECIFIC_TEMPLATES_MATRIX_PRODUCT_HPP

/* =========================================================================
Copyright (c) 2010-2013, Institute for Microelectronics,
                            Institute for Analysis and Scientific Computing,
                            TU Wien.
Portions of this software are copyright by UChicago Argonne, LLC.

                            -----------------
                ViennaCL - The Vienna Computing Library
                            -----------------

Project Head:    Karl Rupp                   rupp@iue.tuwien.ac.at

(A list of authors and contributors can be found in the PDF manual)

License:         MIT (X11), see file LICENSE in the base directory
============================================================================= */


/** @file viennacl/generator/matrix_product.hpp
*
* Kernel template for the matrix product operation
*/

#include <vector>

#include "viennacl/scheduler/forwards.h"

#include "viennacl/device_specific/templates/template_base.hpp"
#include "viennacl/device_specific/mapped_objects.hpp"
#include "viennacl/device_specific/utils.hpp"
#include "viennacl/device_specific/tree_parsing/read_write.hpp"
#include "viennacl/device_specific/tree_parsing/filter.hpp"
#include "viennacl/device_specific/tree_parsing/evaluate_expression.hpp"
#include "viennacl/forwards.h"

#include "viennacl/tools/tools.hpp"

namespace viennacl{

namespace device_specific{


class matrix_product_template : public template_base{

public:
    struct parameters : public template_base::parameters
    {
      parameters(unsigned int simd_width
                 , unsigned int local_size_0, unsigned int KL, unsigned int local_size_1
                 , unsigned int ms, unsigned int ks, unsigned int ns
                 , bool use_A_local, bool use_B_local
                 , unsigned int local_fetch_0, unsigned int local_fetch_1): template_base::parameters(simd_width, local_size_0, local_size_1, 1),
                                             kL(KL), mS(ms), kS(ks), nS(ns), use_A_local(use_A_local), use_B_local(use_B_local),
                                             local_fetch_0(local_fetch_0), local_fetch_1(local_fetch_1),
                                              mL(ms*local_size_0), nL(ns*local_size_1){}

      unsigned int kL;

      unsigned int mS;
      unsigned int kS;
      unsigned int nS;

      bool use_A_local;
      bool use_B_local;

      unsigned int local_fetch_0;
      unsigned int local_fetch_1;

      unsigned int mL;
      unsigned int nL;
    };

private:

    unsigned int n_lmem_elements() const
    {
        unsigned int N = 0;
        if(p_.use_A_local)
            N += p_.kL * (p_.mL+1);
        if(p_.use_B_local)
            N += p_.nL * (p_.kL+1);
        return N;
    }

    void check_invalid_impl(viennacl::ocl::device const & /*device*/) const
    {
        static const unsigned int alignment = 128;
        bool res = false;
        res |= alignment % p_.mL > 0;
        res |= alignment % p_.kL > 0;
        res |= alignment % p_.nL > 0;
        res |= (p_.mS % p_.simd_width) > 0;
        res |= (p_.nS % p_.simd_width) > 0;
        res |= p_.mS > p_.mL;
        res |= p_.nS > p_.nL;
        res |= p_.kS > p_.kL;
        res |= (!(A_trans_=='N' && B_trans_=='T') && p_.simd_width>1);
        if(p_.use_A_local)
        {
            unsigned int bound1 = (A_trans_=='N')?p_.kL:p_.mL;
            unsigned int bound0 = (A_trans_=='N')?p_.mL:p_.kL;

            res |= p_.local_fetch_1>0 && (bound1 % p_.local_fetch_1)> 0;
            res |= p_.local_fetch_0>0 && (bound0 % (p_.local_fetch_0*p_.simd_width)) > 0;
        }
        if(p_.use_B_local)
        {
            unsigned int bound1 = (B_trans_=='T')?p_.kL:p_.nL;
            unsigned int bound0 = (B_trans_=='T')?p_.nL:p_.kL;

            res |= p_.local_fetch_1>0 && (bound1 % p_.local_fetch_1)> 0;
            res |= p_.local_fetch_0>0 && (bound0 % (p_.local_fetch_0*p_.simd_width)) > 0;
        }

        if(p_.use_A_local || p_.use_B_local)
            res |= ((p_.local_fetch_0*p_.local_fetch_1) !=(p_.local_size_0*p_.local_size_1));
    }


    void configure_impl(vcl_size_t /*kernel_id*/, viennacl::ocl::context & /*context*/, statements_container const & statements, viennacl::ocl::kernel & k, unsigned int & n_arg) const
    {
        using namespace device_specific::utils;
        using namespace tree_parsing;

        scheduler::statement const & st = statements.data().front();

        vcl_size_t C_idx=0, A_idx=0, B_idx=0, dummyidx=0;
        leaf_t C_leaf=LHS_NODE_TYPE, A_leaf=LHS_NODE_TYPE, B_leaf=LHS_NODE_TYPE, dummyleaf=LHS_NODE_TYPE;
        parse(st, C_idx, C_leaf, dummyidx, dummyleaf, A_idx, A_leaf, B_idx, B_leaf, dummyidx, dummyleaf);

        //set M, N
        vcl_size_t iM = call_on_matrix(utils::lhs_rhs_element(st, C_idx, C_leaf), internal_size1_fun());
        vcl_size_t iN = call_on_matrix(utils::lhs_rhs_element(st, C_idx, C_leaf), internal_size2_fun());

        vcl_size_t M = call_on_matrix(utils::lhs_rhs_element(st, C_idx, C_leaf), size1_fun());
        vcl_size_t N = call_on_matrix(utils::lhs_rhs_element(st, C_idx, C_leaf), size2_fun());

        vcl_size_t A1 = call_on_matrix(utils::lhs_rhs_element(st, A_idx, A_leaf), size1_fun());
        vcl_size_t A2 = call_on_matrix(utils::lhs_rhs_element(st, A_idx, A_leaf), size2_fun());
        vcl_size_t B1 = call_on_matrix(utils::lhs_rhs_element(st, B_idx, B_leaf), size1_fun());
        vcl_size_t B2 = call_on_matrix(utils::lhs_rhs_element(st, B_idx, B_leaf), size2_fun());

        //set ND range
        k.global_work_size(0, iM/p_.mS);
        k.global_work_size(1, iN/p_.nS);

        k.arg(n_arg++, cl_uint(M));
        k.arg(n_arg++, cl_uint(N));
        if(A1==B1 || A1==B2)
           k.arg(n_arg++, cl_uint(A1));
        else
           k.arg(n_arg++, cl_uint(A2));
    }

    void add_kernel_arguments(statements_container const & /*statements*/, std::string & arguments_string) const
    {
        arguments_string += generate_value_kernel_argument("unsigned int", "M");
        arguments_string += generate_value_kernel_argument("unsigned int", "N");
        arguments_string += generate_value_kernel_argument("unsigned int", "K");
    }


    static void parse(scheduler::statement const & s,
               vcl_size_t & C_idx, tree_parsing::leaf_t & C_leaf, vcl_size_t & alpha_idx, tree_parsing::leaf_t & alpha_leaf,
               vcl_size_t & A_idx, tree_parsing::leaf_t & A_leaf, vcl_size_t & B_idx, tree_parsing::leaf_t & B_leaf,
               vcl_size_t & beta_idx, tree_parsing::leaf_t & beta_leaf)
    {
      using namespace tree_parsing;
      using namespace scheduler;

      scheduler::statement::container_type const & array = s.array();
      vcl_size_t root_idx = s.root();

      C_idx = root_idx;
      C_leaf = LHS_NODE_TYPE;

      vcl_size_t node_add_idx = array[root_idx].rhs.node_index;

      vcl_size_t node_1_idx = array[node_add_idx].lhs.node_index;
      alpha_idx = node_1_idx;
      alpha_leaf = RHS_NODE_TYPE;

      vcl_size_t mat_prod_idx = array[node_1_idx].lhs.node_index;
      if(array[mat_prod_idx].lhs.type_family==MATRIX_TYPE_FAMILY)
        A_idx = mat_prod_idx;
      else
        A_idx = array[mat_prod_idx].lhs.node_index;
      A_leaf = LHS_NODE_TYPE;

      if(array[mat_prod_idx].rhs.type_family==MATRIX_TYPE_FAMILY)
      {
        B_idx = mat_prod_idx;
        B_leaf = RHS_NODE_TYPE;
      }
      else
      {
        B_idx = array[mat_prod_idx].rhs.node_index;
        B_leaf = LHS_NODE_TYPE;
      }

      vcl_size_t node_2_idx = array[node_add_idx].rhs.node_index;
      beta_idx = node_2_idx;
      beta_leaf = RHS_NODE_TYPE;
    }

    void set_simd_widths(scheduler::statement const & s, mapping_type const & m)
    {
      tree_parsing::traverse(s, s.array()[s.array()[s.root()].rhs.node_index].lhs.node_index, set_simd_width_traversal<mapped_matrix>(p_.simd_width, m), true);

    }

    void core(unsigned int /*kernel_id*/, utils::kernel_generation_stream& stream, statements_container const & statements, std::vector<mapping_type> const & mappings) const
    {
        using namespace tree_parsing;

        //////////////////
        /// INIT
        /// //////////////
        scheduler::statement const & st = statements.data().front();
        mapping_type const & mapping = mappings.front();

        vcl_size_t C_idx=0, alpha_idx=0, A_idx=0, B_idx=0, beta_idx=0;
        leaf_t C_leaf=LHS_NODE_TYPE, alpha_leaf=LHS_NODE_TYPE, A_leaf=LHS_NODE_TYPE, B_leaf=LHS_NODE_TYPE, beta_leaf=LHS_NODE_TYPE;
        parse(st, C_idx, C_leaf, alpha_idx, alpha_leaf, A_idx, A_leaf, B_idx, B_leaf, beta_idx, beta_leaf);

        mapped_matrix * C = (mapped_matrix*)mapping.at(mapping_key(C_idx, C_leaf)).get();
        mapped_host_scalar * alpha = (mapped_host_scalar*)mapping.at(mapping_key(alpha_idx, alpha_leaf)).get();
        mapped_matrix * A = (mapped_matrix*)mapping.at(mapping_key(A_idx, A_leaf)).get();
        mapped_matrix * B = (mapped_matrix*)mapping.at(mapping_key(B_idx, B_leaf)).get();
        mapped_host_scalar * beta = (mapped_host_scalar*)mapping.at(mapping_key(beta_idx, beta_leaf)).get();

        if(p_.simd_width>1)
        {
            stream << A->ld() << "/=" << p_.simd_width << ";" << std::endl;
            stream << B->ld() << "/=" << p_.simd_width << ";" << std::endl;
        }


        std::string C_scalartype = C->scalartype();
        std::string A_scalartype = p_.use_A_local?A->scalartype():utils::simd_scalartype(A->scalartype(), p_.simd_width);
        std::string B_scalartype = p_.use_B_local?B->scalartype():utils::simd_scalartype(B->scalartype(), p_.simd_width);

        //////////////////
        /// DECLARATIONS
        /// //////////////


        ///Result Values
        stream << C_scalartype << " " << "rC[" << p_.mS << "][" << p_.nS <<"]  = {(" << C->scalartype() << ")0};" << std::endl;
        stream << A_scalartype << " " << "rA[" << p_.kS << "][" << (p_.use_A_local?p_.mS:p_.mS/p_.simd_width) << "];" << std::endl;
        stream << B_scalartype << " " << "rB[" << p_.kS << "][" << (p_.use_B_local?p_.nS:p_.nS/p_.simd_width) <<"];" << std::endl;
        stream << std::endl;

        if(p_.use_A_local)
            stream << "__local " << A->scalartype() << " lA[" << p_.kL * (p_.mL + 1) << "];" << std::endl;
        if(p_.use_B_local)
            stream << "__local " << B->scalartype() << " lB[" << p_.kL * (p_.nL + 1) << "];" << std::endl;
        stream << std::endl;

        stream << "uint gidx = get_group_id(0);" << std::endl;
        stream << "uint gidy = get_group_id(1);" << std::endl;
        stream << "uint idx = get_local_id(0);" << std::endl;
        stream << "uint idy = get_local_id(1);" << std::endl;
        if(p_.use_A_local || p_.use_B_local){
            stream << std::endl;
            stream << "uint idt = " << p_.local_size_0 << "*idy + idx;" << std::endl;
            stream << "uint idxT = idt % " << p_.local_fetch_0 << ";" << std::endl;
            stream << "uint idyT = idt / " << p_.local_fetch_0 << ";" << std::endl;
        }
        stream << std::endl;

        if(p_.use_A_local)
        {
            if(A_trans_=='N')
                stream << A->name() << " +=  gidx*" << p_.mL/p_.simd_width << "+ idxT + idyT*" << A->ld()  << ";" << std::endl;
            else
                stream << A->name() << " +=  gidx*" << p_.mL/p_.simd_width << "*" << A->ld() << "+ idxT + idyT*" << A->ld()  << ";" << std::endl;
        }
        else
        {
            if(A_trans_=='N')
                stream << A->name() << " += gidx*" << p_.mL/p_.simd_width << "+ idx" << ";" << std::endl;
            else
                stream << A->name() << " += (gidx*" << p_.mL/p_.simd_width << "+ idx)*" << A->ld() << ";" << std::endl;
        }

        if(p_.use_B_local)
        {
            if(B_trans_=='T')
                stream << B->name() << " +=  gidy*" << p_.nL/p_.simd_width << "+ idxT + idyT*" << B->ld()  << ";" << std::endl;
            else
                stream << B->name() << " +=  gidy*" << p_.nL/p_.simd_width << "*" << B->ld() << "+ idxT + idyT*" << B->ld()  << ";" << std::endl;
        }
        else
        {
            if(B_trans_=='T')
                stream << B->name() << " +=  gidy*" << p_.nL/p_.simd_width << "+ idy;" << std::endl;
            else
                stream << B->name() << " += (gidy*" << p_.nL/p_.simd_width << "+ idy)*" << B->ld() << ";" << std::endl;
        }

        stream << std::endl;

        stream << "for(unsigned int block_k=0 ; block_k< K ; block_k+=" << p_.kL << "){" << std::endl;
        stream.inc_tab();

        if(p_.use_A_local){
            if(A_trans_=='N')
                stream << "__local " << A->scalartype() << "* plA = lA + idyT*" << p_.mL+1 << "+" << p_.simd_width << "*idxT;" << std::endl;
            else
                stream << "__local " << A->scalartype() << "* plA = lA + idxT*" << p_.mL+1 << "+ idyT;" << std::endl;
        }


        if(p_.use_B_local)
        {
            if(B_trans_=='T')
                stream << "__local " << B->scalartype() << "* plB = lB + idyT*" << p_.nL+1 << "+" << p_.simd_width << "*idxT;" << std::endl;
            else
                stream << "__local " << B->scalartype() << "* plB = lB + idxT*" << p_.nL+1 << "+ idyT;" << std::endl;
        }


        if(p_.use_A_local || p_.use_B_local)
            stream << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

        ///Fetch LHS to Local Memory
        if(p_.use_A_local)
        {
            unsigned int bound1 = (A_trans_=='N')?p_.kL:p_.mL;
            unsigned int bound0 = (A_trans_=='N')?p_.mL:p_.kL;
            for(unsigned int k = 0 ; k < bound1 ; k += p_.local_fetch_1){
                for(unsigned int m = 0 ; m < bound0 ; m += p_.local_fetch_0*p_.simd_width){
                    unsigned int offset = (A_trans_=='N')?(k*(p_.mL+1)+m):(m*(p_.mL+1)+k);
                    if(p_.simd_width==1)
                        stream << "plA[" << offset << "] = " << A->name() << "[" << m/p_.simd_width <<  "+"  << k << "*" << A->ld() << "];" << std::endl;
                    else
                        stream << "vstore" << p_.simd_width << "(" <<  A->name() << "[" << m/p_.simd_width <<  "+"  << k << "*" << A->ld() << "],0,plA+" << offset << ");" << std::endl;
                }
            }
        }

        ///Fetch RHS to Local Memory
        if(p_.use_B_local)
        {
            unsigned int bound1 = (B_trans_=='T')?p_.kL:p_.nL;
            unsigned int bound0 = (B_trans_=='T')?p_.nL:p_.kL;
            for(unsigned int k = 0 ; k < bound1 ; k += p_.local_fetch_1){
                for(unsigned int n = 0 ; n < bound0 ; n += p_.local_fetch_0*p_.simd_width){
                    unsigned int offset = (B_trans_=='T')?k*(p_.nL+1) + n:n*(p_.nL+1) + k;
                    if(p_.simd_width==1)
                        stream << "plB[" << offset << "] = " << B->name() << "[" << n/p_.simd_width <<  "+"  << k << "*" << B->ld() << "];" << std::endl;
                    else
                        stream << "vstore"  << p_.simd_width << "(" <<  B->name() << "[" << n/p_.simd_width <<  "+"  << k << "*" << B->ld() << "],0,plB+" << offset << ");" << std::endl;
                }
            }
        }

        if(p_.use_A_local || p_.use_B_local)
            stream << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

        stream << "uint offA = " << p_.simd_width << "*idx;" << std::endl;
        stream << "uint offB = " << p_.simd_width << "*idy;" << std::endl;

        //stream << "#pragma unroll" << std::endl;
        stream << "for(unsigned int k = 0 ; k < " << p_.kL << "; k+=" << p_.kS << "){" << std::endl;
        stream.inc_tab();

        ///Fetch LHS to registers
        for(unsigned int kk = 0 ; kk < p_.kS ; ++kk){
            for(unsigned int mm = 0 ; mm < p_.mS/p_.simd_width ; ++mm){
                if(p_.use_A_local)
                    for(unsigned int ss = 0 ; ss < p_.simd_width ; ++ss)
                        stream << "rA[" << kk << "][" << mm*p_.simd_width + ss << "] = lA[offA + " << mm*p_.local_size_0*p_.simd_width + ss + kk*(p_.mL+1) << "];" << std::endl;
                else
                    if(A_trans_=='N')
                        stream << "rA[" << kk << "][" << mm << "] = " << A->name() << "[" << mm*p_.local_size_0 << "+" << kk << "*" << A->ld() << "];" << std::endl;
                    else
                        stream << "rA[" << kk << "][" << mm << "] = " << A->name() << "[" << kk << "+" << mm*p_.local_size_0 << "*" << A->ld() << "];" << std::endl;
            }
        }


            ///Fetch RHS to registers
        for(unsigned int kk = 0 ; kk < p_.kS ; ++kk){
            for(unsigned int nn=0 ; nn < p_.nS/p_.simd_width ; ++nn){
                if(p_.use_B_local)
                    for(unsigned int ss = 0 ; ss < p_.simd_width ; ++ss)
                        stream << "rB[" << kk << "][" << nn*p_.simd_width + ss << "] = lB[offB + " << nn*p_.local_size_1*p_.simd_width + ss + kk*(p_.nL+1) << "];" << std::endl;
                else
                    if(B_trans_=='T')
                        stream << "rB[" << kk << "][" << nn << "] = " << B->name() << "[" << nn*p_.local_size_1 << " + " << kk << "*" << B->ld() << "];" << std::endl;
                    else
                        stream << "rB[" << kk << "][" << nn << "] = " << B->name() << "[" << kk << "+" << nn*p_.local_size_1 << "*" << B->ld() << "];" << std::endl;

            }
        }

        ///Increment pointers
        if(p_.use_A_local)
            stream << "offA += " << p_.kS*(p_.mL+1) << ";" << std::endl;
        else
            if(A_trans_=='N')
                stream << A->name() << " += " << p_.kS << "*" << A->ld() << ";" << std::endl;
            else
                stream << A->name() << " += " << p_.kS << ";" << std::endl;

        if(p_.use_B_local)
            stream << "offB += " << p_.kS*(p_.nL+1) << ";" << std::endl;
        else
            if(B_trans_=='T')
                stream << B->name() << " += " << p_.kS << "*" << B->ld() << ";" << std::endl;
            else
                stream << B->name() << " += " << p_.kS << ";" << std::endl;


        for(unsigned int kk = 0 ; kk < p_.kS ; ++kk)
            for(unsigned int nn=0 ; nn < p_.nS ; ++nn)
                for(unsigned int mm=0 ; mm < p_.mS ; ++mm)
                {
                    std::string res_str, lhs_str, rhs_str;
                    res_str = "rC[" + tools::to_string(mm) + "][" + tools::to_string(nn) + "]";
                    if(p_.use_A_local || p_.simd_width==1)
                        lhs_str = "rA[" + tools::to_string(kk) + "][" + tools::to_string(mm) + "]";
                    else
                        lhs_str = "rA[" + tools::to_string(kk) + "][" + tools::to_string(mm/p_.simd_width) + "].s" + tools::to_string(mm%p_.simd_width);
                    if(p_.use_B_local || p_.simd_width==1)
                        rhs_str = "rB[" + tools::to_string(kk) + "]["+tools::to_string(nn)+"]";
                    else
                        rhs_str = "rB[" + tools::to_string(kk) + "]["+tools::to_string(nn/p_.simd_width)+"].s"+tools::to_string(nn%p_.simd_width);
                    stream << res_str << "=" << "fma(" << lhs_str << "," << rhs_str << "," << res_str << ");" << std::endl;
                }


        stream.dec_tab();
        stream << "}" << std::endl;

        if(p_.use_A_local){
            if(A_trans_=='N')
                stream << A->name() << " += " << p_.kL << "*" << A->ld() << ";" << std::endl;
            else
                stream << A->name() << " += " << p_.kL << ";" << std::endl;
        }

        if(p_.use_B_local){
            if(B_trans_=='T')
                stream << B->name() << " += " << p_.kL << "*" << B->ld() << ";" << std::endl;
            else
                stream << B->name() << " += " << p_.kL << ";" << std::endl;
        }

        stream.dec_tab();
        stream << "}" << std::endl;


        if(C->row_major())
        {
          stream << C->name() << "+= gidx*" << p_.mL << "*" << C->ld() << ";" << std::endl;
          stream << C->name() << "+= idx*" << p_.simd_width << "*" << C->ld() << ";" << std::endl;
          stream << C->name() << "+= gidy*" << p_.nL << ";" << std::endl;
          stream << C->name() << "+= idy*" << p_.simd_width << ";" << std::endl;
          for(unsigned int n=0 ; n < p_.nS ; ++n){
              for(unsigned int m=0 ; m < p_.mS ; ++m){
                  std::string j = tools::to_string((m/p_.simd_width)*(p_.local_size_0*p_.simd_width) + m%p_.simd_width);
                  stream << C->name() << "[" << j << "*" << C->ld() << "]"
                            << "= rC[" << m <<"][" << n << "]*" << alpha->name() << "+ " << C->name() << "[" << j << "*" << C->ld() << "]*" << beta->name() << ";" << std::endl;
              }
              if((n+1)%p_.simd_width>0)
                  stream << C->name() << "+=1;" << std::endl;
              else
                  stream << C->name() << "+=" << (p_.local_size_1*p_.simd_width) - (p_.simd_width-1) << ";" << std::endl;
          }

        }
        else
        {
          stream << C->name() << "+= gidx*" << p_.mL << ";" << std::endl;
          stream << C->name() << "+= idx*" << p_.simd_width << ";" << std::endl;
          stream << C->name() << "+= gidy*" << p_.nL << "*" << C->ld() << ";" << std::endl;
          stream << C->name() << "+= idy*" << p_.simd_width << "*" << C->ld() << ";" << std::endl;
          for(unsigned int m=0 ; m < p_.mS ; ++m)
          {
              for(unsigned int n=0 ; n < p_.nS ; ++n)
              {
                  std::string j = tools::to_string((n/p_.simd_width)*(p_.local_size_1*p_.simd_width) + n%p_.simd_width);
                  stream << C->name() << "[" << j << "*" << C->ld() << "]"
                            << "= rC[" << m <<"][" << n << "]*" << alpha->name() << "+ " << C->name() << "[" << j << "*" << C->ld() << "]*" << beta->name() << ";" << std::endl;
              }

              if((m+1)%p_.simd_width>0)
                  stream << C->name() << "+=1;" << std::endl;
              else
                  stream << C->name() << "+=" << (p_.local_size_0*p_.simd_width) - (p_.simd_width-1) << ";" << std::endl;
          }
        }


    }

public:
    matrix_product_template(matrix_product_template::parameters const & parameters, char A_trans, char B_trans, std::string const & kernel_prefix) : template_base(parameters, kernel_prefix, BIND_ALL_UNIQUE), A_trans_(A_trans), B_trans_(B_trans), p_(parameters){ }

private:
    const char A_trans_;
    const char B_trans_;
    matrix_product_template::parameters const & p_;
};

}

}

#endif