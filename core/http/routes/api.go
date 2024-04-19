package routes

import (
	"github.com/go-skynet/LocalAI/core/config"
	"github.com/go-skynet/LocalAI/core/http/endpoints/elevenlabs"
	"github.com/go-skynet/LocalAI/core/http/endpoints/localai"
	"github.com/go-skynet/LocalAI/core/http/endpoints/openai"
	"github.com/go-skynet/LocalAI/core/services"
	"github.com/go-skynet/LocalAI/internal"
	"github.com/go-skynet/LocalAI/pkg/model"
	"github.com/gofiber/fiber/v2"
	"github.com/gofiber/swagger"
)

func RegisterAPIRoutes(appConfig *config.ApplicationConfig, cl *config.BackendConfigLoader, ml *model.ModelLoader, app *fiber.App, auth fiber.Handler, galleryService *services.GalleryService) {

	modelGalleryEndpointService := localai.CreateModelGalleryEndpointService(appConfig.Galleries, appConfig.ModelPath, galleryService)
	app.Post("/models/apply", auth, modelGalleryEndpointService.ApplyModelGalleryEndpoint())
	app.Get("/models/available", auth, modelGalleryEndpointService.ListModelFromGalleryEndpoint())
	app.Get("/models/galleries", auth, modelGalleryEndpointService.ListModelGalleriesEndpoint())
	app.Post("/models/galleries", auth, modelGalleryEndpointService.AddModelGalleryEndpoint())
	app.Delete("/models/galleries", auth, modelGalleryEndpointService.RemoveModelGalleryEndpoint())
	app.Get("/models/jobs/:uuid", auth, modelGalleryEndpointService.GetOpStatusEndpoint())
	app.Get("/models/jobs", auth, modelGalleryEndpointService.GetAllStatusEndpoint())

	app.Post("/tts", auth, localai.TTSEndpoint(cl, ml, appConfig))

	// Elevenlabs
	app.Post("/v1/text-to-speech/:voice-id", auth, elevenlabs.TTSEndpoint(cl, ml, appConfig))

	// Stores
	sl := model.NewModelLoader("")
	app.Post("/stores/set", auth, localai.StoresSetEndpoint(sl, appConfig))
	app.Post("/stores/delete", auth, localai.StoresDeleteEndpoint(sl, appConfig))
	app.Post("/stores/get", auth, localai.StoresGetEndpoint(sl, appConfig))
	app.Post("/stores/find", auth, localai.StoresFindEndpoint(sl, appConfig))

	// openAI compatible API endpoint

	// chat
	app.Post("/v1/chat/completions", auth, openai.ChatEndpoint(cl, ml, appConfig))
	app.Post("/chat/completions", auth, openai.ChatEndpoint(cl, ml, appConfig))

	// edit
	app.Post("/v1/edits", auth, openai.EditEndpoint(cl, ml, appConfig))
	app.Post("/edits", auth, openai.EditEndpoint(cl, ml, appConfig))

	// assistant
	app.Get("/v1/assistants", auth, openai.ListAssistantsEndpoint(cl, ml, appConfig))
	app.Get("/assistants", auth, openai.ListAssistantsEndpoint(cl, ml, appConfig))
	app.Post("/v1/assistants", auth, openai.CreateAssistantEndpoint(cl, ml, appConfig))
	app.Post("/assistants", auth, openai.CreateAssistantEndpoint(cl, ml, appConfig))
	app.Delete("/v1/assistants/:assistant_id", auth, openai.DeleteAssistantEndpoint(cl, ml, appConfig))
	app.Delete("/assistants/:assistant_id", auth, openai.DeleteAssistantEndpoint(cl, ml, appConfig))
	app.Get("/v1/assistants/:assistant_id", auth, openai.GetAssistantEndpoint(cl, ml, appConfig))
	app.Get("/assistants/:assistant_id", auth, openai.GetAssistantEndpoint(cl, ml, appConfig))
	app.Post("/v1/assistants/:assistant_id", auth, openai.ModifyAssistantEndpoint(cl, ml, appConfig))
	app.Post("/assistants/:assistant_id", auth, openai.ModifyAssistantEndpoint(cl, ml, appConfig))
	app.Get("/v1/assistants/:assistant_id/files", auth, openai.ListAssistantFilesEndpoint(cl, ml, appConfig))
	app.Get("/assistants/:assistant_id/files", auth, openai.ListAssistantFilesEndpoint(cl, ml, appConfig))
	app.Post("/v1/assistants/:assistant_id/files", auth, openai.CreateAssistantFileEndpoint(cl, ml, appConfig))
	app.Post("/assistants/:assistant_id/files", auth, openai.CreateAssistantFileEndpoint(cl, ml, appConfig))
	app.Delete("/v1/assistants/:assistant_id/files/:file_id", auth, openai.DeleteAssistantFileEndpoint(cl, ml, appConfig))
	app.Delete("/assistants/:assistant_id/files/:file_id", auth, openai.DeleteAssistantFileEndpoint(cl, ml, appConfig))
	app.Get("/v1/assistants/:assistant_id/files/:file_id", auth, openai.GetAssistantFileEndpoint(cl, ml, appConfig))
	app.Get("/assistants/:assistant_id/files/:file_id", auth, openai.GetAssistantFileEndpoint(cl, ml, appConfig))

	// files
	app.Post("/v1/files", auth, openai.UploadFilesEndpoint(cl, appConfig))
	app.Post("/files", auth, openai.UploadFilesEndpoint(cl, appConfig))
	app.Get("/v1/files", auth, openai.ListFilesEndpoint(cl, appConfig))
	app.Get("/files", auth, openai.ListFilesEndpoint(cl, appConfig))
	app.Get("/v1/files/:file_id", auth, openai.GetFilesEndpoint(cl, appConfig))
	app.Get("/files/:file_id", auth, openai.GetFilesEndpoint(cl, appConfig))
	app.Delete("/v1/files/:file_id", auth, openai.DeleteFilesEndpoint(cl, appConfig))
	app.Delete("/files/:file_id", auth, openai.DeleteFilesEndpoint(cl, appConfig))
	app.Get("/v1/files/:file_id/content", auth, openai.GetFilesContentsEndpoint(cl, appConfig))
	app.Get("/files/:file_id/content", auth, openai.GetFilesContentsEndpoint(cl, appConfig))

	// completion
	app.Post("/v1/completions", auth, openai.CompletionEndpoint(cl, ml, appConfig))
	app.Post("/completions", auth, openai.CompletionEndpoint(cl, ml, appConfig))
	app.Post("/v1/engines/:model/completions", auth, openai.CompletionEndpoint(cl, ml, appConfig))

	// embeddings
	app.Post("/v1/embeddings", auth, openai.EmbeddingsEndpoint(cl, ml, appConfig))
	app.Post("/embeddings", auth, openai.EmbeddingsEndpoint(cl, ml, appConfig))
	app.Post("/v1/engines/:model/embeddings", auth, openai.EmbeddingsEndpoint(cl, ml, appConfig))

	// audio
	app.Post("/v1/audio/transcriptions", auth, openai.TranscriptEndpoint(cl, ml, appConfig))
	app.Post("/v1/audio/speech", auth, localai.TTSEndpoint(cl, ml, appConfig))

	// images
	app.Post("/v1/images/generations", auth, openai.ImageEndpoint(cl, ml, appConfig))

	ok := func(c *fiber.Ctx) error {
		return c.SendStatus(200)
	}

	// Kubernetes health checks
	app.Get("/healthz", ok)
	app.Get("/readyz", ok)

	// Experimental Backend Statistics Module
	backendMonitor := services.NewBackendMonitor(cl, ml, appConfig) // Split out for now
	app.Get("/backend/monitor", auth, localai.BackendMonitorEndpoint(backendMonitor))
	app.Post("/backend/shutdown", auth, localai.BackendShutdownEndpoint(backendMonitor))

	// models
	app.Get("/v1/models", auth, openai.ListModelsEndpoint(cl, ml))
	app.Get("/models", auth, openai.ListModelsEndpoint(cl, ml))

	app.Get("/metrics", auth, localai.LocalAIMetricsEndpoint())

	app.Get("/swagger/*", swagger.HandlerDefault) // default

	app.Get("/version", auth, func(c *fiber.Ctx) error {
		return c.JSON(struct {
			Version string `json:"version"`
		}{Version: internal.PrintableVersion()})
	})

}
