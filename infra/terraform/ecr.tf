locals {
  images = toset(["napple-online", "napple-room"])
}

resource "aws_ecr_repository" "image" {
  for_each             = local.images
  name                 = each.key
  image_tag_mutability = "IMMUTABLE"
  force_delete         = var.ecr_force_delete

  image_scanning_configuration {
    scan_on_push = true
  }
  encryption_configuration {
    encryption_type = "AES256"
  }
}

resource "aws_ecr_lifecycle_policy" "image" {
  for_each   = aws_ecr_repository.image
  repository = each.value.name
  policy = jsonencode({
    rules = [{
      rulePriority = 1
      description  = "keep the newest ${var.ecr_keep_images} images"
      selection = {
        tagStatus   = "any"
        countType   = "imageCountMoreThan"
        countNumber = var.ecr_keep_images
      }
      action = { type = "expire" }
    }]
  })
}

resource "aws_cloudwatch_log_group" "servers" {
  name              = "/napple/servers"
  retention_in_days = var.log_retention_days
}
