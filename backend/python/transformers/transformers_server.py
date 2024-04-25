#!/usr/bin/env python3
"""
Extra gRPC server for HuggingFace AutoModel models.
"""
from concurrent import futures

import argparse
import signal
import sys
import os
from threading import Thread
import asyncio

import time
import backend_pb2
import backend_pb2_grpc

import grpc
import torch
import torch.cuda


XPU=os.environ.get("XPU", "0") == "1"
if XPU:
    from transformers import AutoTokenizer, AutoModel, set_seed, TextIteratorStreamer
else:
    from transformers import AutoTokenizer, AutoModel, AutoModelForCausalLM, set_seed, BitsAndBytesConfig, TextIteratorStreamer


_ONE_DAY_IN_SECONDS = 60 * 60 * 24

# If MAX_WORKERS are specified in the environment use it, otherwise default to 1
MAX_WORKERS = int(os.environ.get('PYTHON_GRPC_MAX_WORKERS', '1'))


def mean_pooling(model_output, attention_mask):
    """
    Mean pooling to get sentence embeddings. See:
    https://huggingface.co/sentence-transformers/paraphrase-distilroberta-base-v1
    """
    token_embeddings = model_output[0]
    input_mask_expanded = attention_mask.unsqueeze(-1).expand(token_embeddings.size()).float()
    sum_embeddings = torch.sum(token_embeddings * input_mask_expanded, 1) # Sum columns
    sum_mask = torch.clamp(input_mask_expanded.sum(1), min=1e-9)
    return sum_embeddings / sum_mask

# Implement the BackendServicer class with the service methods
class BackendServicer(backend_pb2_grpc.BackendServicer):
    """
    A gRPC servicer for the backend service.

    This class implements the gRPC methods for the backend service, including Health, LoadModel, and Embedding.
    """
    def Health(self, request, context):
        """
        A gRPC method that returns the health status of the backend service.

        Args:
            request: A HealthRequest object that contains the request parameters.
            context: A grpc.ServicerContext object that provides information about the RPC.

        Returns:
            A Reply object that contains the health status of the backend service.
        """
        return backend_pb2.Reply(message=bytes("OK", 'utf-8'))

    def LoadModel(self, request, context):
        """
        A gRPC method that loads a model into memory.

        Args:
            request: A LoadModelRequest object that contains the request parameters.
            context: A grpc.ServicerContext object that provides information about the RPC.

        Returns:
            A Result object that contains the result of the LoadModel operation.
        """
        model_name = request.Model

        compute = "auto"
        if request.F16Memory == True:
            compute=torch.bfloat16

        self.CUDA = request.CUDA
        self.OV=False

        device_map="cpu"

        quantization = None

        if self.CUDA:
            if request.Device:
                device_map=request.Device
            else:
                device_map="cuda:0"
            if request.Quantization == "bnb_4bit":
                quantization = BitsAndBytesConfig(
                    load_in_4bit = True,
                    bnb_4bit_compute_dtype = compute,
                    bnb_4bit_quant_type = "nf4",
                    bnb_4bit_use_double_quant = True,
                    load_in_8bit = False,
                )
            elif request.Quantization == "bnb_8bit":
                quantization = BitsAndBytesConfig(
                    load_in_4bit=False,
                    bnb_4bit_compute_dtype = None,
                    load_in_8bit=True,                                   
                )
                                               
        try:
            if request.Type == "AutoModelForCausalLM":
                if XPU:
                    import intel_extension_for_pytorch as ipex
                    from intel_extension_for_transformers.transformers.modeling import AutoModelForCausalLM

                    device_map="xpu"
                    compute=torch.float16
                    if request.Quantization == "xpu_4bit":
                        xpu_4bit = True
                        xpu_8bit = False
                    elif request.Quantization == "xpu_8bit":
                        xpu_4bit = False
                        xpu_8bit = True
                    else:
                        xpu_4bit = False
                        xpu_8bit = False
                    self.model = AutoModelForCausalLM.from_pretrained(model_name, 
                                                                      trust_remote_code=request.TrustRemoteCode, 
                                                                      use_safetensors=True,
                                                                      device_map=device_map, 
                                                                      load_in_4bit=xpu_4bit, 
                                                                      load_in_8bit=xpu_8bit, 
                                                                      torch_dtype=compute)
                else:
                    self.model = AutoModelForCausalLM.from_pretrained(model_name, 
                                                                      trust_remote_code=request.TrustRemoteCode, 
                                                                      use_safetensors=True, 
                                                                      quantization_config=quantization, 
                                                                      device_map=device_map, 
                                                                      torch_dtype=compute)
            elif request.Type == "OVModelForCausalLM":
                from optimum.intel.openvino import OVModelForCausalLM
                from openvino.runtime import Core

                if "GPU" in Core().available_devices:
                    device_map="GPU"
                else:
                    device_map="CPU"
                self.model = OVModelForCausalLM.from_pretrained(model_name, 
                                                                compile=True,
                                                                trust_remote_code=request.TrustRemoteCode,
                                                                ov_config={"PERFORMANCE_HINT": "CUMULATIVE_THROUGHPUT"}, 
                                                                device=device_map)
                self.OV = True
            else:
                self.model = AutoModel.from_pretrained(model_name, 
                                                       trust_remote_code=request.TrustRemoteCode,  
                                                       use_safetensors=True,  
                                                       quantization_config=quantization, 
                                                       device_map=device_map, 
                                                       torch_dtype=compute)
            if request.ContextSize > 0:
                self.max_tokens = request.ContextSize
            else:
                self.max_tokens = self.model.config.max_position_embeddings
 
            self.tokenizer = AutoTokenizer.from_pretrained(model_name, use_safetensors=True)
            self.XPU = False

            if XPU and self.OV == False:
                self.XPU = True
                try:
                    print("Optimizing model", model_name, "to XPU.", file=sys.stderr)
                    self.model = ipex.optimize_transformers(self.model, inplace=True, dtype=torch.float16, device="xpu")
                except Exception as err:
                    print("Not using XPU:", err, file=sys.stderr)

        except Exception as err:
            print("Error:", err, file=sys.stderr)
            return backend_pb2.Result(success=False, message=f"Unexpected {err=}, {type(err)=}")
        # Implement your logic here for the LoadModel service
        # Replace this with your desired response
        return backend_pb2.Result(message="Model loaded successfully", success=True)

    def Embedding(self, request, context):
        """
        A gRPC method that calculates embeddings for a given sentence.

        Args:
            request: An EmbeddingRequest object that contains the request parameters.
            context: A grpc.ServicerContext object that provides information about the RPC.

        Returns:
            An EmbeddingResult object that contains the calculated embeddings.
        """

        set_seed(request.Seed)
        # Tokenize input
        max_length = 512
        if request.Tokens != 0:
            max_length = request.Tokens
        encoded_input = self.tokenizer(request.Embeddings, padding=True, truncation=True, max_length=max_length, return_tensors="pt")    

        # Create word embeddings
        if self.CUDA:
            encoded_input = encoded_input.to("cuda")

        with torch.no_grad():    
            model_output = self.model(**encoded_input)

        # Pool to get sentence embeddings; i.e. generate one 1024 vector for the entire sentence
        sentence_embeddings = mean_pooling(model_output, encoded_input['attention_mask'])
        print("Calculated embeddings for: " + request.Embeddings, file=sys.stderr)
        print("Embeddings:", sentence_embeddings, file=sys.stderr)
        return backend_pb2.EmbeddingResult(embeddings=sentence_embeddings[0])

    async def _predict(self, request, context, streaming=False): 
        set_seed(request.Seed)
        if request.TopP == 0:
            request.TopP = 0.9
        
        if request.TopK == 0:
            request.TopK = 40

        prompt = request.Prompt
        if not request.Prompt and request.UseTokenizerTemplate and request.Messages:    
            prompt = self.tokenizer.apply_chat_template(request.Messages, tokenize=False, add_generation_prompt=True)

        eos_token_id = self.tokenizer.eos_token_id
        if request.StopPrompts:
            eos_token_id = []
            for word in request.StopPrompts:
                eos_token_id.append(self.tokenizer.convert_tokens_to_ids(word))

        inputs = self.tokenizer(prompt, return_tensors="pt")

        if request.Tokens > 0:
            max_tokens = request.Tokens
        else:
            max_tokens = self.max_tokens - inputs["input_ids"].size()[inputs["input_ids"].dim()-1]

        if self.CUDA:
            inputs = inputs.to("cuda")
        if XPU and self.OV == False:
            inputs = inputs.to("xpu")
            streaming = False

        if streaming:
            streamer=TextIteratorStreamer(self.tokenizer,
                                        skip_prompt=True,
                                        skip_special_tokens=True)
            config=dict(inputs,
                        max_new_tokens=max_tokens, 
                        temperature=request.Temperature, 
                        top_p=request.TopP,
                        top_k=request.TopK, 
                        do_sample=True,
                        attention_mask=inputs["attention_mask"],
                        eos_token_id=eos_token_id,
                        pad_token_id=self.tokenizer.eos_token_id,
                        streamer=streamer)
            thread=Thread(target=self.model.generate, kwargs=config)
            thread.start()
            generated_text = ""
            try:
                for new_text in streamer:
                    generated_text += new_text
                    yield backend_pb2.Reply(message=bytes(new_text, encoding='utf-8'))
            finally:
                thread.join()
        else:
            if XPU and self.OV == False:
                outputs = self.model.generate(inputs["input_ids"],
                                    max_new_tokens=max_tokens, 
                                    temperature=request.Temperature, 
                                    top_p=request.TopP,
                                    top_k=request.TopK, 
                                    do_sample=True,
                                    pad_token=self.tokenizer.eos_token_id)
            else:
                outputs = self.model.generate(inputs["input_ids"],
                        max_new_tokens=max_tokens, 
                        temperature=request.Temperature, 
                        top_p=request.TopP,
                        top_k=request.TopK, 
                        do_sample=True,
                        attention_mask=inputs["attention_mask"],
                        eos_token_id=eos_token_id,
                        pad_token_id=self.tokenizer.eos_token_id)
            generated_text = self.tokenizer.batch_decode(outputs[:, inputs["input_ids"].shape[1]:], skip_special_tokens=True)[0]

        if streaming:
            return

        yield backend_pb2.Reply(message=bytes(generated_text, encoding='utf-8'))

    async def Predict(self, request, context):
        """
        Generates text based on the given prompt and sampling parameters.

        Args:
            request: The predict request.
            context: The gRPC context.

        Returns:
            backend_pb2.Reply: The predict result.
        """
        gen = self._predict(request, context, streaming=False)
        res = await gen.__anext__()
        return res

    async def PredictStream(self, request, context):
        """
        Generates text based on the given prompt and sampling parameters, and streams the results.

        Args:
            request: The predict stream request.
            context: The gRPC context.

        Returns:
            backend_pb2.Result: The predict stream result.
        """
        iterations = self._predict(request, context, streaming=True)
        try:
            async for iteration in iterations:
                yield iteration
        finally:
            await iterations.aclose()

async def serve(address):
    # Start asyncio gRPC server
    server = grpc.aio.server(migration_thread_pool=futures.ThreadPoolExecutor(max_workers=MAX_WORKERS))
    # Add the servicer to the server
    backend_pb2_grpc.add_BackendServicer_to_server(BackendServicer(), server)
    # Bind the server to the address
    server.add_insecure_port(address)

    # Gracefully shutdown the server on SIGTERM or SIGINT
    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(
            sig, lambda: asyncio.ensure_future(server.stop(5))
        )

    # Start the server
    await server.start()
    print("Server started. Listening on: " + address, file=sys.stderr)
    # Wait for the server to be terminated
    await server.wait_for_termination()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run the gRPC server.")
    parser.add_argument(
        "--addr", default="localhost:50051", help="The address to bind the server to."
    )
    args = parser.parse_args()

    asyncio.run(serve(args.addr))