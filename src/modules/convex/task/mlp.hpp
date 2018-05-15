/* ----------------------------------------------------------------------- *//**
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
 *
 * @file mlp.hpp
 *
 * This file contains objective function related computation, which is called
 * by classes in algo/, e.g.,  loss, gradient functions
 *
 *//* ----------------------------------------------------------------------- */

#ifndef MADLIB_MODULES_CONVEX_TASK_MLP_HPP_
#define MADLIB_MODULES_CONVEX_TASK_MLP_HPP_

#include <dbconnector/dbconnector.hpp>

namespace madlib {

namespace modules {

namespace convex {

// Use Eigen
using namespace madlib::dbal::eigen_integration;

template <class Model, class Tuple>
class MLP {
public:
    typedef Model model_type;
    typedef Tuple tuple_type;
    typedef typename Tuple::independent_variables_type
        independent_variables_type;
    typedef typename Tuple::dependent_variable_type dependent_variable_type;

    static void gradientInPlace(
            model_type                          &model,
            const independent_variables_type    &x,
            const dependent_variable_type       &y,
            const double                        &stepsize);

    static double getLossAndUpdateModel(
            model_type                          &model,
            const Matrix                        &x,
            const Matrix                        &y,
            const double                        &stepsize);

    static double getLossAndGradient(
            const model_type                    &model,
            const Matrix                        &x,
            const Matrix                        &y,
            std::vector<Matrix>                 &total_gradient_per_layer,
            const double                        stepsize);

    static double getLoss(
            const ColumnVector                  &y_true,
            const ColumnVector                  &y_estimated,
            const bool                          is_classification);

    static double loss(
            const model_type                    &model,
            const independent_variables_type    &x,
            const dependent_variable_type       &y);

    static ColumnVector predict(
            const model_type                    &model,
            const independent_variables_type    &x,
            const bool                          is_classification_response,
            const bool                          is_dep_var_array_for_classification);

    const static int RELU = 0;
    const static int SIGMOID = 1;
    const static int TANH = 2;
    static double lambda;

private:
    static double sigmoid(const double &xi) {
        return 1. / (1. + std::exp(-xi));
    }

    static double relu(const double &xi) {
        return xi*(xi>0);
    }

    static double tanh(const double &xi) {
        return std::tanh(xi);
    }

    static double sigmoidDerivative(const double &xi) {
        double value = sigmoid(xi);
        return value * (1. - value);
    }

    static double reluDerivative(const double &xi) {
        return xi>0;
    }

    static double tanhDerivative(const double &xi) {
        double value = tanh(xi);
        return 1-value*value;
    }

    static void feedForward(
            const model_type                    &model,
            const independent_variables_type    &x,
            std::vector<ColumnVector>           &net,
            std::vector<ColumnVector>           &o);

    static void backPropogate(
            const ColumnVector                  &y_true,
            const ColumnVector                  &y_estimated,
            const std::vector<ColumnVector>     &net,
            const model_type                    &model,
            std::vector<ColumnVector>           &delta);
};

template <class Model, class Tuple>
double MLP<Model, Tuple>::lambda = 0;

template <class Model, class Tuple>
double
MLP<Model, Tuple>::getLossAndUpdateModel(
        model_type           &model,
        const Matrix         &x_batch,
        const Matrix         &y_true_batch,
        const double         &stepsize) {

    float mu = 0.9;
    std::vector<Matrix> total_gradient_per_layer(model.num_layers);

    // model is updated with the momentum step (i.e. velocity vector)
    // if Nesterov Accelerated Gradient is enabled
    model.nesterovUpdate();
    double total_loss = getLossAndGradient(model,
                                           x_batch, y_true_batch,
                                           total_gradient_per_layer, stepsize);

    model.updateVelocity(total_gradient_per_layer);
    model.updatePosition(total_gradient_per_layer);
    return total_loss;
}

template <class Model, class Tuple>
double
MLP<Model, Tuple>::getLossAndGradient(
        const model_type     &model,
        const Matrix         &x_batch,
        const Matrix         &y_true_batch,
        std::vector<Matrix>  &total_gradient_per_layer,
        const double         stepsize) {

    double total_loss = 0.;
    // gradient and loss added over the batch
    for (size_t k=0; k < model.num_layers; ++k) {
        total_gradient_per_layer[k] = Matrix::Zero(model.u[k].rows(),
                                                   model.u[k].cols());
    }
    size_t num_rows_in_batch = x_batch.rows();
    for (size_t i=0; i < num_rows_in_batch; i++){
        ColumnVector x = x_batch.row(i);
        ColumnVector y_true = y_true_batch.row(i);

        std::vector<ColumnVector> net, o, delta;
        feedForward(model, x, net, o);
        backPropogate(y_true, o.back(), net, model, delta);

        // compute the gradient
        for (size_t k=0; k < model.num_layers; k++){
            total_gradient_per_layer[k] += o[k] * delta[k].transpose();
        }

        // compute the loss
        total_loss += getLoss(y_true, o.back(), model.is_classification);
    }

    // convert gradient to a gradient update vector
    //  1. normalize to per row update
    //  2. discount by stepsize
    //  3. add regularization
    //  4. make negative
    for (size_t k=0; k < model.num_layers; k++){
        Matrix regularization = MLP<Model, Tuple>::lambda * model.u[k];
        regularization.row(0).setZero(); // Do not update bias
        total_gradient_per_layer[k] = -stepsize * total_gradient_per_layer[k] / num_rows_in_batch +
                                            regularization;
    }
    return total_loss;
}

template <class Model, class Tuple>
double
MLP<Model, Tuple>::getLoss(const ColumnVector &y_true,
                           const ColumnVector &y_estimated,
                           const bool is_classification){
    if(is_classification){
        // softmax loss function
        double clip = 1.e-10;
        ColumnVector y_clipped = y_estimated.cwiseMax(clip).cwiseMin(1. - clip);
        return -(y_true.array() * y_clipped.array().log() +
                    (1 - y_true.array()) * (1 - y_clipped.array()).log()
                ).sum();
    }
    else{
        // squared loss
        return 0.5 * (y_estimated - y_true).squaredNorm();
    }
}

template <class Model, class Tuple>
void
MLP<Model, Tuple>::gradientInPlace(
        model_type                          &model,
        const independent_variables_type    &x,
        const dependent_variable_type       &y_true,
        const double                        &stepsize)
{
    // getLossAndUpdateModel expects a batch for x and y where each row of x and y
    // is a single data point.
    // x and y_true in gradientInPlace are of type ColumnVector(see tuple.hpp)
    // and we transpose them to pass them as a row.
    getLossAndUpdateModel(model, x.transpose(), y_true.transpose(), stepsize);
}

template <class Model, class Tuple>
double
MLP<Model, Tuple>::loss(
        const model_type                    &model,
        const independent_variables_type    &x,
        const dependent_variable_type       &y_true) {

    // Here we compute the loss. In the case of regression we use sum of square errors
    // In the case of classification the loss term is cross entropy.
    std::vector<ColumnVector> net, o;
    feedForward(model, x, net, o);
    ColumnVector y_estimated = o.back();

    if(model.is_classification){
        double clip = 1.e-10;
        y_estimated = y_estimated.cwiseMax(clip).cwiseMin(1.-clip);
        return - (y_true.array()*y_estimated.array().log()
               + (-y_true.array()+1)*(-y_estimated.array()+1).log()).sum();
    }
    else{
        return 0.5 * (y_estimated - y_true).squaredNorm();
    }
}

template <class Model, class Tuple>
ColumnVector
MLP<Model, Tuple>::predict(
        const model_type                    &model,
        const independent_variables_type    &x,
        const bool                          is_classification_response,
        const bool                          is_dep_var_array_for_classification) {
    std::vector<ColumnVector> net, o;

    feedForward(model, x, net, o);
    ColumnVector output = o.back();

    if(is_classification_response){
        int max_idx;
        output.maxCoeff(&max_idx);
        if(is_dep_var_array_for_classification) {
            // Return the entire array, but with 1 for the class level with
            // largest probability and 0s for the rest.
            output.setZero();
            output[max_idx] = 1;
        } else {
            // Return a length 1 array with the predicted index
            output.resize(1);
            output[0] = (double) max_idx;
        }
    }
    return output;
}


template <class Model, class Tuple>
void
MLP<Model, Tuple>::feedForward(
        const model_type                    &model,
        const independent_variables_type    &x,
        std::vector<ColumnVector>           &net,
        std::vector<ColumnVector>           &o){
    uint16_t k, N;
    /*
        The network starts with the 0th layer (input), followed by n_layers
        number of hidden layers, and then an output layer.
    */
    // Total number of coefficients in the model
    N = model.u.size(); // assuming >= 1
    net.resize(N + 1);
    // o[k] is a vector of the output of the kth layer
    o.resize(N + 1);

    double (*activation)(const double&);
    if(model.activation==RELU)
        activation = &relu;
    else if(model.activation==SIGMOID)
        activation = &sigmoid;
    else
        activation = &tanh;

    o[0].resize(x.size() + 1);
    o[0] << 1.,x;

    for (k = 1; k < N; k ++) {
        // o_k = activation(sum(o_{k-1} * u_{k-1}))
        // net_k just does the inner sum: input to the activation function
        net[k] = model.u[k-1].transpose() * o[k-1];
        o[k] = ColumnVector(model.u[k-1].cols() + 1);
        // This applies the activation function to give the actual node output
        o[k] << 1., net[k].unaryExpr(activation);
    }
    o[N] = model.u[N-1].transpose() * o[N-1];

    // Numerically stable calculation of softmax
    if(model.is_classification){
        double max_x = o[N].maxCoeff();
        o[N] = (o[N].array() - max_x).exp();
        o[N] /= o[N].sum();
    }
}

template <class Model, class Tuple>
void
MLP<Model, Tuple>::backPropogate(
        const ColumnVector                  &y_true,
        const ColumnVector                  &y_estimated,
        const std::vector<ColumnVector>     &net,
        const model_type                    &model,
        std::vector<ColumnVector>           &delta) {
    uint16_t k, N;
    N = model.u.size(); // assuming >= 1
    delta.resize(N);

    double (*activationDerivative)(const double&);
    if(model.activation==RELU)
        activationDerivative = &reluDerivative;
    else if(model.activation==SIGMOID)
        activationDerivative = &sigmoidDerivative;
    else
        activationDerivative = &tanhDerivative;

    delta.back() = y_estimated - y_true;
    for (k = N - 1; k >= 1; k --) {
        // Do not include the bias terms
        delta[k-1] = model.u[k].bottomRows(model.u[k].rows()-1) * delta[k];
        delta[k-1] = delta[k-1].array() * net[k].unaryExpr(activationDerivative).array();
    }
}

} // namespace convex

} // namespace modules

} // namespace madlib

#endif


