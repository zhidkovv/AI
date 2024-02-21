package localai

import (
	"github.com/go-skynet/LocalAI/core/backend"
	"github.com/go-skynet/LocalAI/core/config"
	fiberContext "github.com/go-skynet/LocalAI/core/http/ctx"
	"github.com/go-skynet/LocalAI/pkg/model"

	"github.com/go-skynet/LocalAI/core/schema"
	"github.com/gofiber/fiber/v2"
	"github.com/rs/zerolog/log"
)

func TTSEndpoint(cl *config.ConfigLoader, ml *model.ModelLoader, o *schema.StartupOptions) func(c *fiber.Ctx) error {
	return func(c *fiber.Ctx) error {

		input := new(schema.TTSRequest)

		// Get input data from the request body
		if err := c.BodyParser(input); err != nil {
			return err
		}

		modelFile, err := fiberContext.ModelFromContext(c, ml, input.Model, false)
		if err != nil {
			modelFile = input.Model
			log.Warn().Msgf("Model not found in context: %s", input.Model)
		}
		cfg, err := config.Load(modelFile, o.ModelPath, cl, false, 0, 0, false)
		if err != nil {
			modelFile = input.Model
			log.Warn().Msgf("Model not found in context: %s", input.Model)
		} else {
			modelFile = cfg.Model
		}
		log.Debug().Msgf("Request for model: %s", modelFile)

		if input.Backend != "" {
			cfg.Backend = input.Backend
		}

		filePath, _, err := backend.ModelTTS(cfg.Backend, input.Input, modelFile, ml, o, *cfg)
		if err != nil {
			return err
		}
		return c.Download(filePath)
	}
}
