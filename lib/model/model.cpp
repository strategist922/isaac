#include <set>
#include <fstream>
#include <stdexcept>

#include "rapidjson/document.h"
#include "atidlas/backend/parse.h"
#include "atidlas/backend/templates/vaxpy.h"
#include "atidlas/backend/templates/reduction.h"
#include "atidlas/backend/templates/maxpy.h"
#include "atidlas/backend/templates/mreduction.h"
#include "atidlas/backend/templates/mproduct.h"
#include "atidlas/exception/unknown_datatype.h"
#include "atidlas/exception/operation_not_supported.h"
#include "atidlas/model/model.h"
#include "atidlas/tools/make_vector.hpp"
#include "atidlas/tools/timer.hpp"
#include "convert.hpp"


namespace atidlas
{


std::string model::define_extension(std::string const & extensions, std::string const & ext)
{
  if(extensions.find(ext)!=std::string::npos)
    return std::string("#pragma OPENCL EXTENSION " + ext + " : enable\n");
  return std::string("");
}

void model::fill_program_name(char* program_name, symbolic_expressions_container const & symbolic_expressions, binding_policy_t binding_policy)
{
  if (symbolic_expressions.order()==symbolic_expressions_container::INDEPENDENT)
    *program_name++='i';
  else
    *program_name++='s';
  symbolic_binder* binder = NULL;
  if(binding_policy==BIND_TO_HANDLE)
    binder = new bind_to_handle();
  else
    binder = new bind_all_unique();
  for (symbolic_expressions_container::data_type::const_iterator it = symbolic_expressions.data().begin(); it != symbolic_expressions.data().end(); ++it)
    traverse(**it, (*it)->root(), symbolic_expression_representation_functor(*binder, program_name),true);
  *program_name='\0';
  delete binder;
}

std::vector<cl_ext::lazy_compiler>& model::init(symbolic_expressions_container const & symbolic_expressions, cl::Context const & context, cl::Device const & device, bool force_recompilation)
{
  char program_name[256];
  fill_program_name(program_name, symbolic_expressions, BIND_TO_HANDLE);
  std::string pname(program_name);
  std::vector<cl_ext::lazy_compiler> & to_init = lazy_programs_[context()][pname];
  if(to_init.empty())
  {
    std::string extensions = device.getInfo<CL_DEVICE_EXTENSIONS>();

    to_init.push_back(cl_ext::lazy_compiler(context, pname, force_recompilation));
    to_init.back().add(define_extension(extensions, "cl_khr_fp64"));

    to_init.push_back(cl_ext::lazy_compiler(context, pname + "_fb", force_recompilation));
    to_init.back().add(define_extension(extensions, "cl_khr_fp64"));

    for(size_t i = 0 ; i < templates_.size() ; ++i)
    {
      std::vector<std::string> cur = templates_[i]->generate(i, symbolic_expressions, device);
      for(size_t j = 0 ; j < cur.size() ; ++j){
        to_init[j].add(cur[j]);
      }
    }
  }
  return to_init;
}

model::model(predictors::random_forest const & predictor, std::vector< tools::shared_ptr<base> > const & templates, cl::CommandQueue & queue) :
  templates_(templates), predictor_(new predictors::random_forest(predictor)), queue_(queue)
{}

model::model(std::vector< tools::shared_ptr<base> > const & templates, cl::CommandQueue & queue) :  templates_(templates), queue_(queue)
{}

model::model(base const & tp, cl::CommandQueue & queue) : templates_(1,tp.clone()), queue_(queue)
{}

void model::execute(symbolic_expressions_container const & symbolic_expressions, bool bypass_predictor, bool force_recompilation)
{
  bypass_predictor = bypass_predictor || predictor_.get()==NULL;
  cl::Context const & context = symbolic_expressions.context();
  assert(context() == queue_.getInfo<CL_QUEUE_CONTEXT>()());
  cl::Device const & device = queue_.getInfo<CL_QUEUE_DEVICE>();

  std::vector<cl_ext::lazy_compiler> & compilers = init(symbolic_expressions, context, device, force_recompilation);

  //Prediction
  std::vector<int_t> x = templates_[0]->input_sizes(symbolic_expressions);
  int label;
  //The user tuned the model specifically for this input size
  if(hardcoded_.find(x)!=hardcoded_.end())
    label = hardcoded_.at(x);
  //The user bypasses the random forest
  else if(bypass_predictor)
    label = 0;
  //Default
  else
  {
    std::vector<float> predictions = predictor_->predict(x);
    label = std::distance(predictions.begin(),std::min_element(predictions.begin(), predictions.end()));
  }

  //Execution
  templates_[label]->enqueue(queue_, compilers, label, symbolic_expressions);
}

void model::tune(symbolic_expressions_container const & symbolic_expressions)
{
  cl::Context const & context = symbolic_expressions.context();
  assert(context() == queue_.getInfo<CL_QUEUE_CONTEXT>()());
  cl::Device device = queue_.getInfo<CL_QUEUE_DEVICE>();

  std::vector<cl_ext::lazy_compiler> & compilers = init(symbolic_expressions, context, device, false);

  //Collect the timings
  std::vector<float> timings(templates_.size());
  tools::timer timer;
  for(size_t i = 0 ; i < templates_.size() ; ++i)
  {
    timer.start();
    templates_[i]->enqueue(queue_, compilers, i, symbolic_expressions);
    queue_.finish();
    timings[i] = timer.get();
  }

  //Fill the override
  std::vector<int_t> x = templates_[0]->input_sizes(symbolic_expressions);
  hardcoded_[x] = std::distance(timings.begin(),std::min_element(timings.begin(), timings.end()));
}

model::templates_container const & model::templates() const
{ return templates_; }

///////////////////

namespace detail
{
  static expression_type get_expression_type(std::string const & name)
  {
    if(name=="vaxpy") return VECTOR_AXPY_TYPE;
    if(name=="dot") return REDUCTION_TYPE;
    if(name=="maxpy") return MATRIX_AXPY_TYPE;
    if(name=="gemvN") return ROW_WISE_REDUCTION_TYPE;
    if(name=="gemvT") return COL_WISE_REDUCTION_TYPE;
    if(name=="gemmNN") return MATRIX_PRODUCT_NN_TYPE;
    if(name=="gemmNT") return MATRIX_PRODUCT_NT_TYPE;
    if(name=="gemmTN") return MATRIX_PRODUCT_TN_TYPE;
    if(name=="gemmTT") return MATRIX_PRODUCT_TT_TYPE;
    throw std::invalid_argument("Invalid expression: " + name);
  }

  static numeric_type get_dtype(std::string const & name)
  {
    if(name=="float32") return FLOAT_TYPE;
    if(name=="float64") return DOUBLE_TYPE;
    throw std::invalid_argument("Invalid datatype: " + name);
  }

  static tools::shared_ptr<base> create(std::string const & template_name, std::vector<int> const & a)
  {
    fetching_policy_type fetch[] = {FETCH_FROM_LOCAL, FETCH_FROM_GLOBAL_STRIDED, FETCH_FROM_GLOBAL_CONTIGUOUS};
    if(template_name=="vaxpy")
      return tools::shared_ptr<base>(new vaxpy(a[0], a[1], a[2], fetch[a[3]]));
    else if(template_name=="dot")
      return tools::shared_ptr<base>(new reduction(a[0], a[1], a[2], fetch[a[3]]));
    else if(template_name=="maxpy")
      return tools::shared_ptr<base>(new maxpy(a[0], a[1], a[2], a[3], a[4], fetch[a[5]]));
    else if(template_name.find("gemvN")!=std::string::npos)
      return tools::shared_ptr<base>(new mreduction_rows(a[0], a[1], a[2], a[3], fetch[a[4]]));
    else if(template_name.find("gemvT")!=std::string::npos)
      return tools::shared_ptr<base>(new mreduction_cols(a[0], a[1], a[2], a[3], fetch[a[4]]));
    else if(template_name.find("gemmNN")!=std::string::npos)
      return tools::shared_ptr<base>(new mproduct_nn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], fetch[a[7]], fetch[a[8]], a[9], a[10]));
    else if(template_name.find("gemmTN")!=std::string::npos)
      return tools::shared_ptr<base>(new mproduct_tn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], fetch[a[7]], fetch[a[8]], a[9], a[10]));
    else if(template_name.find("gemmNT")!=std::string::npos)
      return tools::shared_ptr<base>(new mproduct_nt(a[0], a[1], a[2], a[3], a[4], a[5], a[6], fetch[a[7]], fetch[a[8]], a[9], a[10]));
    else if(template_name.find("gemmTT")!=std::string::npos)
      return tools::shared_ptr<base>(new mproduct_tt(a[0], a[1], a[2], a[3], a[4], a[5], a[6], fetch[a[7]], fetch[a[8]], a[9], a[10]));
    else
      throw std::invalid_argument("Invalid expression: " + template_name);
  }
}

void import(std::string const & fname, cl::CommandQueue & queue, model_map_t& result)
{
  namespace js = rapidjson;
  //Parse the JSON document
  js::Document document;
  std::ifstream t(fname.c_str());
  std::string str;
  t.seekg(0, std::ios::end);
  str.reserve(t.tellg());
  t.seekg(0, std::ios::beg);
  str.assign((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
  document.Parse<0>(str.c_str());
  //Deserialize
  std::vector<std::string> operations = tools::make_vector<std::string>() << "vaxpy" << "dot"
                                                                          << "maxpy" << "gemvN" << "gemvT"
                                                                          << "gemmNN" << "gemmTN" << "gemmTT";
  std::vector<std::string> dtype = tools::make_vector<std::string>() << "float32" << "float64";
  for(std::vector<std::string>::iterator op = operations.begin() ; op != operations.end() ; ++op)
  {
    const char * opcstr = op->c_str();
    if(document.HasMember(opcstr))
    {
      expression_type etype = detail::get_expression_type(*op);
      for(std::vector<std::string>::iterator dt = dtype.begin() ; dt != dtype.end() ; ++dt)
      {
        const char * dtcstr = dt->c_str();
        if(document[opcstr].HasMember(dtcstr))
        {
          numeric_type dtype = detail::get_dtype(*dt);

          // Get profiles
          std::vector<tools::shared_ptr<base> > templates;
          js::Value const & profiles = document[opcstr][dtcstr]["profiles"];
          for (js::SizeType id = 0 ; id < profiles.Size() ; ++id)
            templates.push_back(detail::create(*op, tools::to_int_array<int>(profiles[id])));
          if(templates.size()>1)
          {
            // Get predictor
            predictors::random_forest predictor(document[opcstr][dtcstr]["predictor"]);
            result[std::make_pair(etype, dtype)] = tools::shared_ptr<model>(new model(predictor, templates, queue));
          }
          else
            result[std::make_pair(etype, dtype)] = tools::shared_ptr<model>(new model(templates, queue));
        }
      }
    }
  }
}

model_map_t init_models(cl::CommandQueue & queue)
{
  model_map_t res;
  typedef tools::shared_ptr<model> ptr_t;
  numeric_type types[] = {CHAR_TYPE, UCHAR_TYPE, SHORT_TYPE, USHORT_TYPE, INT_TYPE, UINT_TYPE, LONG_TYPE, ULONG_TYPE, FLOAT_TYPE, DOUBLE_TYPE};

  for(size_t i = 0 ; i < 10 ; ++i){
    numeric_type DTYPE = types[i];
    res[std::make_pair(SCALAR_AXPY_TYPE, DTYPE)] = ptr_t(new model(vaxpy(1,64,128,FETCH_FROM_GLOBAL_STRIDED), queue));
    res[std::make_pair(VECTOR_AXPY_TYPE, DTYPE)] = ptr_t (new model(vaxpy(1,64,128,FETCH_FROM_GLOBAL_STRIDED), queue));
    res[std::make_pair(REDUCTION_TYPE, DTYPE)] = ptr_t(new model(reduction(1,64,128,FETCH_FROM_GLOBAL_STRIDED), queue));
    res[std::make_pair(MATRIX_AXPY_TYPE, DTYPE)] = ptr_t(new model(maxpy(1,8,8,8,8,FETCH_FROM_GLOBAL_STRIDED), queue));
    res[std::make_pair(ROW_WISE_REDUCTION_TYPE, DTYPE)] = ptr_t(new model(mreduction_rows(1, 8, 8, 16, FETCH_FROM_GLOBAL_STRIDED), queue));
    res[std::make_pair(COL_WISE_REDUCTION_TYPE, DTYPE)] = ptr_t(new model(mreduction_cols(1, 8, 8, 16, FETCH_FROM_GLOBAL_STRIDED), queue));
    res[std::make_pair(MATRIX_PRODUCT_NN_TYPE, DTYPE)] = ptr_t(new model(mproduct_nn(1, 8, 8, 8, 4, 1, 4, FETCH_FROM_LOCAL, FETCH_FROM_LOCAL, 8, 8), queue));
    res[std::make_pair(MATRIX_PRODUCT_TN_TYPE, DTYPE)] = ptr_t(new model(mproduct_tn(1, 8, 8, 8, 4, 1, 4, FETCH_FROM_LOCAL, FETCH_FROM_LOCAL, 8, 8), queue));
    res[std::make_pair(MATRIX_PRODUCT_NT_TYPE, DTYPE)] = ptr_t(new model(mproduct_nt(1, 8, 8, 8, 4, 1, 4, FETCH_FROM_LOCAL, FETCH_FROM_LOCAL, 8, 8), queue));
    res[std::make_pair(MATRIX_PRODUCT_TT_TYPE, DTYPE)] = ptr_t(new model(mproduct_tt(1, 8, 8, 8, 4, 1, 4, FETCH_FROM_LOCAL, FETCH_FROM_LOCAL, 8, 8), queue));
  }
  if(const char * cmodel_file = std::getenv("ATIDLAS_MODEL_DEVICE_0"))
    import(std::string(cmodel_file), queue, res);
  return res;


  //    else
  //      throw "Please specify a model file";
}

model_map_t& get_model_map(cl::CommandQueue & queue)
{
  std::map<cl::CommandQueue, model_map_t, cl_ext::compare>::iterator it = models.find(queue);
  if(it == models.end())
    return models.insert(std::make_pair(queue, init_models(queue))).first->second;
  return it->second;
}

model& get_model(cl::CommandQueue & queue, expression_type expression, numeric_type dtype)
{
  std::pair<expression_type, numeric_type> key(expression, dtype);
  return *get_model_map(queue).at(key);
}

std::map<cl::CommandQueue, model_map_t, cl_ext::compare> models;

}