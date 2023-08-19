package transformers

// This is a wrapper to statisfy the GRPC service interface
// It is meant to be used by the main executable that is the server for the specific backend type (falcon, gpt3, etc)
import (
	"fmt"

	"github.com/go-skynet/LocalAI/pkg/grpc/base"
	pb "github.com/go-skynet/LocalAI/pkg/grpc/proto"
	"github.com/rs/zerolog/log"

	transformers "github.com/go-skynet/go-ggml-transformers.cpp"
)

type MPT struct {
	base.Base

	mpt *transformers.MPT
}

func (llm *MPT) Load(opts *pb.ModelOptions) error {
	if llm.Base.State != pb.StatusResponse_UNINITIALIZED {
		log.Warn().Msgf("mpt backend loading %s while already in state %s!", opts.Model, llm.Base.State.String())
	}

	llm.Base.Lock()
	defer llm.Base.Unlock()
	model, err := transformers.NewMPT(opts.ModelFile)
	llm.mpt = model
	return err
}

func (llm *MPT) Predict(opts *pb.PredictOptions) (string, error) {
	llm.Base.Lock()
	defer llm.Base.Unlock()

	return llm.mpt.Predict(opts.Prompt, buildPredictOptions(opts)...)
}

// fallback to Predict
func (llm *MPT) PredictStream(opts *pb.PredictOptions, results chan string) error {
	llm.Base.Lock()
	go func() {
		res, err := llm.mpt.Predict(opts.Prompt, buildPredictOptions(opts)...)

		if err != nil {
			fmt.Println("err: ", err)
		}
		results <- res
		close(results)
		llm.Base.Unlock()
	}()
	return nil
}

func (llm *MPT) Unload() error {
	llm.Base.Lock()
	defer llm.Base.Unlock()

	llm.mpt.Free()

	return nil
}
